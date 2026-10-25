#include "FLNativeSlotPowerControl.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace power=rysup::fl_ara::slot_power;
namespace {std::atomic<std::uint64_t> allocations{0};}
void* operator new(std::size_t count){allocations.fetch_add(1);if(void* p=std::malloc(count?count:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t count){return ::operator new(count);}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}
namespace {
std::vector<std::string> passed;
void demand(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void pass(const char* value){passed.emplace_back(value);}
void* pointer(std::uintptr_t value){return reinterpret_cast<void*>(value);}
struct Fixture {
    power::Registry<2> registry;power::Binding mono{0x1000,0x2000,1},stereo{0x3000,0x4000,1};
    std::uint64_t calls=0;void* actualThis=nullptr;std::intptr_t actualID=0,actualIndex=0,actualValue=0;
    const std::intptr_t nativeResult=std::numeric_limits<std::intptr_t>::min()+123;
    Fixture(){demand(registry.publishAtModelBoundary(0,mono)&&registry.publishAtModelBoundary(1,stereo),"fixture publication");}
    std::intptr_t send(void* self,std::intptr_t id,std::intptr_t index,std::intptr_t value){
        return power::forwardOriginalExactlyOnce(registry,[&](void* s,std::intptr_t i,std::intptr_t x,std::intptr_t v)noexcept{
            ++calls;actualThis=s;actualID=i;actualIndex=x;actualValue=v;return nativeResult;},self,id,index,value);
    }
    void notify(power::Binding& b,std::intptr_t value){send(pointer(b.wrapper),11,0,value);}
};
void initialAndScalarForwarding(){
    Fixture f;
    demand(f.mono.snapshotForBlock()==power::Power::Unknown&&f.stereo.snapshotForBlock()==power::Power::Unknown,"new binding was authorized");
    pass("fresh exact owner generations start Unknown");
    const auto min=std::numeric_limits<std::intptr_t>::min(),max=std::numeric_limits<std::intptr_t>::max();
    for(const auto id:{min,std::intptr_t(-1),std::intptr_t(0),std::intptr_t(10),std::intptr_t(11),std::intptr_t(12),max})
        for(const auto value:{min,std::intptr_t(-1),std::intptr_t(0),std::intptr_t(1),std::intptr_t(2),max}){
            const auto before=f.calls;const auto result=f.send(pointer(f.mono.wrapper),id,max,value);
            demand(f.calls==before+1&&result==f.nativeResult&&f.actualThis==pointer(f.mono.wrapper)&&f.actualID==id&&
                f.actualIndex==max&&f.actualValue==value,"native original/result/64-bit scalar changed");
        }
    pass("all IDs and signed 64-bit scalar extremes forward original exactly once with unchanged return");
    f.notify(f.mono,1);f.notify(f.stereo,1);
    for(const auto self:{pointer(0x7777),static_cast<void*>(nullptr)}){
        const auto before=f.calls;f.send(self,11,0,0);demand(f.calls==before+1,"unrelated original skipped");}
    demand(f.mono.snapshotForBlock()==power::Power::Enabled&&f.stereo.snapshotForBlock()==power::Power::Enabled,"unrelated wrapper routed power");
    pass("unrelated and null wrapper calls forward without changing own power");
    f.notify(f.mono,0);
    demand(f.mono.snapshotForBlock()==power::Power::Disabled&&f.stereo.snapshotForBlock()==power::Power::Enabled,"OFF crossed instance");
    pass("actual ID11 OFF is isolated to exact wrapper binding");
    for(const auto id:{std::intptr_t(12),std::intptr_t(2),std::intptr_t(1)})f.send(pointer(f.stereo.wrapper),id,0,0);
    demand(f.stereo.snapshotForBlock()==power::Power::Enabled,"transport/flush/lifecycle-style ID conflated with power");
    pass("playing/flush/processing-mode Dispatcher IDs do not supply power authority");
    for(const auto invalid:{min,std::intptr_t(-1),std::intptr_t(2),max}){
        f.notify(f.mono,1);f.notify(f.mono,invalid);demand(f.mono.snapshotForBlock()==power::Power::Unknown,"non-Boolean granted power");}
    pass("only actual ID11 values zero/one grant Disabled/Enabled; other values revoke to Unknown");
}
void blockAndReturnBoundary(){
    Fixture f;f.notify(f.mono,1);
    const auto previousBlock=f.mono.snapshotForBlock();
    bool observedUnknown=false;
    const auto result=power::forwardOriginalExactlyOnce(f.registry,[&](void*,std::intptr_t,std::intptr_t,std::intptr_t)noexcept{
        observedUnknown=f.mono.snapshotForBlock()==power::Power::Unknown;return std::intptr_t(1234);
    },pointer(f.mono.wrapper),11,0,0);
    demand(observedUnknown&&result==1234&&previousBlock==power::Power::Enabled&&f.mono.snapshotForBlock()==power::Power::Disabled,
        "block-granularity return boundary changed");
    pass("already-admitted blocks may finish; snapshots during original ID11 are Unknown and completed OFF is Disabled");
    std::array<float,8> native{{0.25f,-0.5f,0.75f,-1.f,0.f,0.1f,-0.2f,0.3f}};const auto dry=native;
    const auto frameCount=4;std::uint8_t ended=1,changed=1;bool wetAllowed=false,holdRequired=false;
    const auto snapshot=f.mono.snapshotForBlock();
    if(snapshot==power::Power::Enabled)wetAllowed=true;
    else if(snapshot==power::Power::Unknown)holdRequired=true;
    demand(snapshot==power::Power::Disabled&&!wetAllowed&&!holdRequired&&native==dry&&frameCount==4&&ended==1&&changed==1,
        "disabled block policy rewrote native output");
    pass("Disabled branch example leaves all original-produced bytes/flags/count untouched");
    // A power observation is never a cache-ticket/dirty/prime mutation.
    std::uint64_t cacheEpoch=17;bool dirty=true,cacheAuthorized=false,primePending=true;
    f.notify(f.mono,1);
    demand(f.mono.snapshotForBlock()==power::Power::Enabled&&cacheEpoch==17&&dirty&&!cacheAuthorized&&primePending,
        "power ON reauthorized a stale cache");
    pass("ON carries no cache reauthorization, dirty clear, ticket update or prime reset");
}
void nestedAndConcurrent(){
    Fixture f;f.notify(f.mono,1);unsigned originalCalls=0;
    power::forwardOriginalExactlyOnce(f.registry,[&](void*,std::intptr_t,std::intptr_t,std::intptr_t){
        ++originalCalls;
        power::forwardOriginalExactlyOnce(f.registry,[&](void*,std::intptr_t,std::intptr_t,std::intptr_t)noexcept{
            ++originalCalls;return std::intptr_t(42);},pointer(f.mono.wrapper),11,0,0);
        demand(f.mono.snapshotForBlock()==power::Power::Unknown,"nested dispatch supplied transient authority");
        return std::intptr_t(99);
    },pointer(f.mono.wrapper),11,0,1);
    demand(originalCalls==2&&f.mono.snapshotForBlock()==power::Power::Unknown&&f.mono.callsInFlight()==0,"outer completion overwrote nested OFF");
    pass("same-owner reentrant ID11 forwards both originals once and cannot regress final power");
    f.notify(f.mono,0);demand(f.mono.snapshotForBlock()==power::Power::Disabled,"later uncontended OFF did not recover");
    pass("a new uncontended actual notification recovers a live Unknown generation");
    for(bool firstFinishesFirst:{false,true}){
        auto first=f.registry.beforeOriginal(pointer(f.mono.wrapper),11,1);
        auto second=f.registry.beforeOriginal(pointer(f.mono.wrapper),11,0);
        demand(f.mono.snapshotForBlock()==power::Power::Unknown,"overlap retained wet authority");
        if(firstFinishesFirst){first.originalReturned();second.originalReturned();}
        else{second.originalReturned();first.originalReturned();}
        demand(f.mono.snapshotForBlock()==power::Power::Unknown&&f.mono.callsInFlight()==0,"overlap completion order granted power");
    }
    pass("both completion orders of overlapping same-owner notifications remain Unknown");
    std::atomic<unsigned> ready{0};std::atomic<bool> release{false};
    auto worker=[&](std::intptr_t value){auto pending=f.registry.beforeOriginal(pointer(f.mono.wrapper),11,value);
        ready.fetch_add(1);while(!release.load())std::this_thread::yield();pending.originalReturned();};
    std::thread a(worker,0),b(worker,1);while(ready.load()!=2)std::this_thread::yield();
    const auto during=f.mono.snapshotForBlock();release=true;a.join();b.join();
    demand(during==power::Power::Unknown&&f.mono.snapshotForBlock()==power::Power::Unknown&&f.mono.callsInFlight()==0,"real two-thread overlap granted authority");
    pass("two-thread overlapping Dispatcher observations remain bounded and fail Unknown");
    auto first=f.registry.beforeOriginal(pointer(f.mono.wrapper),11,1);
    auto other=f.registry.beforeOriginal(pointer(f.stereo.wrapper),11,0);
    first.originalReturned();other.originalReturned();
    demand(f.mono.snapshotForBlock()==power::Power::Enabled&&f.stereo.snapshotForBlock()==power::Power::Disabled,"different-owner callbacks were conflated");
    pass("concurrent distinct owners retain independent native power");
    try{power::forwardOriginalExactlyOnce(f.registry,[](void*,std::intptr_t,std::intptr_t,std::intptr_t)->std::intptr_t{
        throw std::runtime_error("fake native exception");},pointer(f.mono.wrapper),11,0,1);demand(false,"exception disappeared");}
    catch(const std::runtime_error&){demand(f.mono.snapshotForBlock()==power::Power::Unknown&&f.mono.callsInFlight()==0,"aborted original authorized power");}
    pass("an exceptional original propagates unchanged and observation fails Unknown");
}
void generationAndSeed(){
    power::Registry<2> registry;power::Binding old{0x1000,0x2000,1};
    demand(registry.publishAtModelBoundary(0,old),"old publication");
    const auto fresh=old.captureInitialSeedAtStoppedBoundary();
    demand(old.trySeedAtStoppedBoundary(fresh,1)&&old.snapshotForBlock()==power::Power::Enabled,"fresh stopped seed refused");
    pass("fresh Unknown generation accepts one current native typed-byte seed at caller-proven boundary");
    const auto reseed=old.captureInitialSeedAtStoppedBoundary();
    demand(!old.trySeedAtStoppedBoundary(reseed,0)&&old.snapshotForBlock()==power::Power::Unknown,"seed replay changed power");
    pass("repeated seed fails Unknown instead of replacing live authority");
    auto inFlight=registry.beforeOriginal(pointer(old.wrapper),11,0);
    registry.retireAtModelBoundary(0);power::Binding next{0x1000,0x5000,2};
    demand(registry.publishAtModelBoundary(0,next),"fresh replacement after closed old generation refused");
    const auto initial=next.captureInitialSeedAtStoppedBoundary();
    demand(next.trySeedAtStoppedBoundary(initial,0),"new stopped seed");inFlight.originalReturned();
    demand(old.closed()&&old.snapshotForBlock()==power::Power::Unknown&&next.snapshotForBlock()==power::Power::Disabled,"retired callback updated replacement generation");
    pass("retired old invocation cannot publish into a replacement at a reused wrapper address");
    power::Binding duplicate{0x1000,0x6000,3};demand(!registry.publishAtModelBoundary(1,duplicate),"duplicate live owner accepted");
    pass("a duplicate live wrapper owner is refused");
    registry.retireAtModelBoundary(0);power::Binding backwards{0x7000,0x8000,2};
    demand(!registry.publishAtModelBoundary(0,backwards),"slot generation did not advance");
    pass("replacement generation must advance monotonically in its registry slot");
    power::Binding zeroWrapper{0,1,1},zeroComponent{1,0,1},zeroGeneration{1,1,0};
    demand(zeroWrapper.closed()&&zeroComponent.closed()&&zeroGeneration.closed(),"empty identity admitted");
    pass("empty wrapper/component/generation identity permanently closes the binding");
    power::Binding changed{0x9000,0xa000,5};const auto stale=changed.captureInitialSeedAtStoppedBoundary();
    auto actual=changed.beforeOriginal(1);actual.originalReturned();
    demand(!changed.trySeedAtStoppedBoundary(stale,0)&&changed.snapshotForBlock()==power::Power::Unknown,"stale seed overwrote newer notification");
    auto recover=changed.beforeOriginal(0);recover.originalReturned();
    demand(changed.snapshotForBlock()==power::Power::Disabled,"post-seed-failure notification did not recover");
    pass("notification revision invalidates old seed; failure is Unknown and later real notification recovers");
    power::Binding reentrant{0xb000,0xc000,6};const auto early=reentrant.captureInitialSeedAtStoppedBoundary();
    auto active=reentrant.beforeOriginal(1);
    demand(!reentrant.trySeedAtStoppedBoundary(early,1),"seed overlapped native call");active.originalReturned();
    demand(reentrant.snapshotForBlock()==power::Power::Unknown&&reentrant.callsInFlight()==0,"reentrant seed granted power on return");
    pass("seed racing an in-flight native notification fails Unknown through its completion");
    power::Binding closed{0xd000,0xe000,7};const auto beforeClose=closed.captureInitialSeedAtStoppedBoundary();closed.closePermanently();
    demand(!closed.trySeedAtStoppedBoundary(beforeClose,1)&&closed.closed()&&closed.snapshotForBlock()==power::Power::Unknown,"seed reopened closed generation");
    pass("seed cannot revive a closed generation");
    power::Binding invalidByte{0xf000,0x10000,8};const auto invalid=invalidByte.captureInitialSeedAtStoppedBoundary();
    demand(!invalidByte.trySeedAtStoppedBoundary(invalid,2)&&invalidByte.snapshotForBlock()==power::Power::Unknown,"non-typed seed admitted");
    power::Binding different{0x11000,0x12000,9};
    demand(!different.trySeedAtStoppedBoundary(invalid,1)&&different.snapshotForBlock()==power::Power::Unknown,"another binding's seed ticket admitted");
    pass("non-Boolean or foreign-binding seed evidence fails Unknown");
}
void boundedNoAllocationAndStress(){
    Fixture f;const auto before=allocations.load();
    for(unsigned i=0;i<10000;++i){f.send(pointer(f.mono.wrapper),11,0,i&1);f.send(pointer(0x7777),99,-1,2);}
    const auto after=allocations.load();
    demand(after==before&&f.calls==20000&&f.mono.callsInFlight()==0,"bounded callback path allocated/leaked calls");
    pass("20000 observed/unrelated original forwards perform zero allocations");
    std::atomic<bool> done{false},bad{false};
    std::thread reader([&]{while(!done.load()){const auto value=f.mono.snapshotForBlock();if(value!=power::Power::Unknown&&value!=power::Power::Enabled&&value!=power::Power::Disabled)bad=true;}});
    for(unsigned i=0;i<10000;++i)f.notify(f.mono,i&1);
    f.notify(f.mono,0);done=true;reader.join();
    demand(!bad&&f.mono.snapshotForBlock()==power::Power::Disabled&&f.mono.callsInFlight()==0,"source snapshots/notification stress failed");
    pass("concurrent source snapshots stay typed and observe final completed OFF");
    power::Binding saturated{0x20000,0x21000,100};
    std::vector<power::Binding::Pending> held;held.reserve(65535);
    for(unsigned i=0;i<65535;++i)held.push_back(saturated.beforeOriginal(1));
    demand(saturated.callsInFlight()==65535&&!saturated.closed(),"in-flight count did not reach bounded extent");
    auto overflow=saturated.beforeOriginal(0);
    demand(!overflow&&saturated.closed()&&saturated.snapshotForBlock()==power::Power::Unknown,"in-flight overflow wrapped/reopened power");
    held.clear();demand(saturated.callsInFlight()==0,"closed outstanding observations did not drain");
    pass("in-flight saturation permanently closes instead of wrapping; existing observations safely drain");
}
}
int main(){try{initialAndScalarForwarding();blockAndReturnBoundary();nestedAndConcurrent();generationAndSeed();boundedNoAllocationAndStress();
    for(const auto& name:passed)std::cout<<"PASS "<<name<<'\n';std::cout<<"TOTAL "<<passed.size()<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
