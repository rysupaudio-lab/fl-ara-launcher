// OWN-file controls only. No DAW/plugin/ARA factory is loaded. Native admission
// below is explicitly stubbed; no test branch is present in a production build.
#define RYSUP_FL_SAVE_GUARD_TESTING 1
#include "FLAppBridge.mm"
#include "../host/StandaloneCacheInvalidation.h"
#include "../host/StandaloneARAEditRelay.h"
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <sys/stat.h>

namespace rysup::fl_ara {
static NativeApplication testApplication;
static std::uint64_t forbiddenSDKCalls=0;
static std::atomic<std::uint64_t> nativeControllerResolverCalls{0};
static bool allowControllerResolution=false;static std::uintptr_t expectedController=0,expectedContext=0;static NativeInsert testControllerInsert;
bool initializeNativeApplication(const NativeApplicationCallbacks&)noexcept{return false;}
const NativeApplication& nativeApplication()noexcept{return testApplication;}
NativeAudioTestStats nativeAudioTestStats()noexcept{return {};}
bool nativeApplicationAdmitted()noexcept{return true;}
bool refreshActualNativeApplication()noexcept{return false;}
bool resolveActualNativeInsert(std::uintptr_t,std::uintptr_t,NativeInsert&)noexcept{return false;}
bool revalidateActualNativeInsert(const NativeInsert&)noexcept{return false;}
bool resolveActualNativeController(std::uintptr_t controller,std::uintptr_t context,NativeInsert& out)noexcept{++nativeControllerResolverCalls;if(!allowControllerResolution||controller!=expectedController||context!=expectedContext)return false;out=testControllerInsert;return true;}
bool revalidateActualNativeController(const NativeInsert& insert,std::uintptr_t controller)noexcept{++nativeControllerResolverCalls;return allowControllerResolution&&controller==expectedController&&insert.actualComponent==testControllerInsert.actualComponent;}
bool readActualNativeTransportState(std::int32_t& value)noexcept{value=0;return true;}
bool withActualNativeMixCaptureLock(const NativeInsert&,const std::function<void()>&){++forbiddenSDKCalls;throw std::runtime_error("unexpected native metadata capture in a rejection-only control");}
bool requestActualNativePlaying(const NativeInsert&,bool)noexcept{++forbiddenSDKCalls;return false;}
bool requestActualNativeProjectDirty(const NativeInsert&)noexcept{++forbiddenSDKCalls;return false;}
void writeNativeApplicationEvent(const char*,const std::string&)noexcept{}
}
namespace {
using namespace rysup::fl_ara;
namespace bridge=rysup::fl_ara::app_bridge;
unsigned checks=0;
void check(bool value,const char* description){++checks;if(!value)throw std::runtime_error(description);}
void seed(const std::string& path,const std::string& bytes){std::ofstream f(path,std::ios::binary);f<<bytes;check(bool(f),"own fixture write failed");}
std::string bytes(const std::string& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
bool exists(const std::string& path){struct stat s{};return lstat(path.c_str(),&s)==0;}
std::atomic<unsigned> ordinaryStateCalls{0};
std::atomic<bool> forceOwnerGuard{true};
Steinberg::tresult PLUGIN_API ordinaryState(void*,Steinberg::IBStream*){ordinaryStateCalls.fetch_add(1);return Steinberg::kResultOk;}
void recognizedStateMustRemainProtected(native_hooks::InstanceRecord& n,native_hooks::StateOperation operation){
    const rysup::ara_lab::Bytes raw{'R','Y','S','F','L','A','P','P'};
    auto* stream=new rysup::ara_lab::fl_app::MemoryIBStream(raw);native_hooks::ThreadInfo info;info.isMainThread=true;
    native_hooks::OriginalStateCall original(&n,ordinaryState);const auto before=ordinaryStateCalls.load();
    check(bridge::state(n,operation,stream,original,info,nullptr)==Steinberg::kResultFalse,"unbound recognized state was accepted");
    check(ordinaryStateCalls==before&&!original.wasCalled(),"unbound recognized envelope reached the original plugin");
    Steinberg::int64 position=-1;check(stream->tell(&position)==Steinberg::kResultOk&&position==0,"refusal consumed recognized native bytes");
    check(stream->bytes()==raw,"refusal modified recognized native bytes");stream->release();
    check(n.ownerStateFlags.load()&bridge::protectedUnboundArchive,"state hook failed to latch unbound protection");
}
void scopedParameterCallbacks(){
    std::array<bridge::BridgeRecord,3> owners;std::array<native_hooks::InstanceRecord,3> components,controllers;
    std::array<bridge::ObservedNativeInstance,3> nodes;native_hooks::InstanceRecord unrelated;bridge::ObservedNativeInstance unrelatedNode;
    auto* shared=reinterpret_cast<Steinberg::Vst::IComponentHandler*>(0x7000);
    auto* other=reinterpret_cast<Steinberg::Vst::IComponentHandler*>(0x7100);
    auto* pending=reinterpret_cast<Steinberg::Vst::IComponentHandler*>(0x7200);
    for(unsigned i=0;i<3;++i){auto& r=owners[i];auto& n=controllers[i];auto& node=nodes[i];r.native=&components[i];
        r.native->nativeInitialized=true;r.native->ownerData=&r;r.nativeCacheGate=std::make_shared<rysup::ara_lab::StandaloneCacheGate>();
        n.nativeInitialized=true;n.actualController=reinterpret_cast<Steinberg::Vst::IEditController*>(0x8000+i*0x100);
        n.actualNativeComponentHandler=i<2?shared:other;node.native=&n;node.verifiedController=n.actualController.load();node.controllerOwner=&r;
        n.ownerControllerData=&node;r.currentControllerNode=&node;r.next=i<2?&owners[i+1]:nullptr;
    }
    unrelated.nativeInitialized=true;unrelated.actualController=reinterpret_cast<Steinberg::Vst::IEditController*>(0x9000);
    unrelatedNode.native=&unrelated;unrelatedNode.verifiedController=unrelated.actualController.load();unrelated.ownerControllerData=&unrelatedNode;
    auto* previous=bridge::runtime().head.exchange(&owners[0]);
    struct Restore {bridge::BridgeRecord* old;~Restore(){bridge::runtime().head=old;}}restore{previous};
    native_hooks::ThreadInfo rt;rt.insideNativeProcess=true; // Explicit pure atomic/RT path.
    const auto resolutionCallsBefore=rysup::fl_ara::nativeControllerResolverCalls.load();
    auto reset=[&]{for(auto& r:owners){const auto ticket=r.nativeCacheGate->invalidate();check(r.nativeCacheGate->publishIfUnchanged(ticket),"callback fixture ticket publication failed");r.dirty=false;r.nativeStopForRebuildPending=false;}};
    auto expect=[&](unsigned mask){for(unsigned i=0;i<3;++i){const auto& r=owners[i];const bool changed=(mask&(1u<<i))!=0;
        check(r.nativeCacheGate->authorizes(r.nativeCacheGate->currentTicket())!=changed,"parameter callback changed an unrelated cache gate");
        check(r.dirty.load()==changed&&r.nativeStopForRebuildPending.load()==changed,"parameter callback queued the wrong ARA stop/rebuild");}};
    reset();bridge::parameterNormalized(unrelated,unrelated.actualController.load(),1,.3,rt,nullptr);expect(0);
    bridge::parameterEdit(&unrelated,reinterpret_cast<Steinberg::Vst::IComponentHandler*>(0x7300),1,.3,rt,nullptr);expect(0);
    reset();bridge::parameterNormalized(controllers[0],controllers[0].actualController.load(),1,.3,rt,nullptr);expect(1);
    reset();bridge::parameterNormalized(controllers[0],unrelated.actualController.load(),1,.3,rt,nullptr);expect(0);
    reset();bridge::parameterEdit(&unrelated,shared,1,.3,rt,nullptr);expect(3); // Diagnostic first-controller is irrelevant.
    reset();controllers[0].actualPendingNativeComponentHandler=pending;bridge::parameterEdit(nullptr,pending,1,.3,rt,nullptr);expect(1);
    reset();controllers[0].actualPendingNativeComponentHandler=nullptr;bridge::parameterEdit(nullptr,pending,1,.3,rt,nullptr);expect(0);
    reset();controllers[0].nativeTerminated=true;bridge::parameterNormalized(controllers[0],controllers[0].actualController.load(),1,.3,rt,nullptr);expect(0);
    bridge::parameterEdit(nullptr,shared,1,.3,rt,nullptr);expect(2);controllers[0].nativeTerminated=false;
    reset();owners[0].currentControllerNode=nullptr;bridge::parameterNormalized(controllers[0],controllers[0].actualController.load(),1,.3,rt,nullptr);expect(0);
    bridge::parameterEdit(nullptr,shared,1,.3,rt,nullptr);expect(2);owners[0].currentControllerNode=&nodes[0];
    reset();native_hooks::InstanceRecord ordinaryComponent;bridge::inputParameterChanges(ordinaryComponent,nullptr,1,128,rt,nullptr);expect(0);
    bridge::inputParameterChanges(components[1],nullptr,0,128,rt,nullptr);expect(0);
    bridge::inputParameterChanges(components[1],nullptr,1,128,rt,nullptr);expect(2);
    check(rysup::fl_ara::forbiddenSDKCalls==0&&rysup::fl_ara::nativeControllerResolverCalls==resolutionCallsBefore,"RT callback controls entered native SDK/capture/VM resolution");
    // Explicit fake native association lets us exercise unchanged publication
    // concurrently with actual production RT callbacks, without reading VM.
    auto& r=owners[0];auto& n=controllers[0];r.identity.actualComponent=0xa000;r.insert.actualComponent=0xa000;
    components[0].actualComponent=reinterpret_cast<Steinberg::Vst::IComponent*>(0xa000);
    n.actualInitializeContext=reinterpret_cast<Steinberg::FUnknown*>(0xa100);
    rysup::fl_ara::allowControllerResolution=true;rysup::fl_ara::expectedController=reinterpret_cast<std::uintptr_t>(n.actualController.load());
    rysup::fl_ara::expectedContext=0xa100;rysup::fl_ara::testControllerInsert=r.insert;
    reset();bridge::publishControllerAssociationOnMain(n);expect(0);
    const auto epochBefore=r.nativeCacheGate->currentTicket().epoch;std::atomic<bool> begin{false};
    constexpr unsigned iterations=5000;
    std::thread producer([&]{while(!begin.load())std::this_thread::yield();for(unsigned i=0;i<iterations;++i)bridge::parameterNormalized(n,n.actualController.load(),1,.3,rt,nullptr);});
    begin=true;for(unsigned i=0;i<iterations;++i)bridge::publishControllerAssociationOnMain(n);producer.join();
    check(r.nativeCacheGate->currentTicket().epoch==epochBefore+iterations,"unchanged main publication lost a concurrent native parameter notification");expect(1);
    reset();rysup::fl_ara::allowControllerResolution=false;bridge::publishControllerAssociationOnMain(n);expect(1);
    check(!nodes[0].controllerOwner.load()&&!r.currentControllerNode.load(),"failed current native proof retained stale controller routing");
    reset();bridge::publishControllerAssociationOnMain(unrelated);expect(0);
}
enum class Admission {RealOwnerPreflight,MissingMember,MissingSecondSerialization};
Admission admission=Admission::RealOwnerPreflight;
std::array<native_hooks::InstanceRecord,2> negativeNativeRecords;
bool ownerPreflight(const NativeSaveRequest& request,void** cookie,void* context)noexcept{
    if(admission==Admission::RealOwnerPreflight)return bridge::nativeSavePreflight(request,cookie,context);
    // Negative fixtures ONLY: intentionally incomplete owner freeze. No prepared
    // authority/archive is forged and no fixture can pass full precommit.
    try{auto& rt=bridge::runtime();rt.nativeSaveFreeze=std::make_unique<bridge::NativeSaveFreeze>();
        auto& freeze=*rt.nativeSaveFreeze;freeze.request=request;freeze.preflightPassed=true;
        const auto count=admission==Admission::MissingMember?1u:2u;
        for(unsigned i=0;i<count;++i){bridge::FrozenNativeSaveComponent entry;entry.record=rt.records.at(i).get();
            entry.serializedCalls=i==0?1:0;freeze.components.push_back(std::move(entry));
            bridge::closeAndDrainNativeSaveComponent(freeze.components.back());}
        *cookie=&freeze;return true;
    }catch(...){return false;}
}
NativeSaveRequest request(const std::string& root,const std::string& name,std::uintptr_t frame){
    NativeSaveRequest out;out.actualSaveFrame=frame;out.targetPath=root+"/"+name+".flp";out.actualOpenPath=out.targetPath+"~";return out;
}
void finishFrame(){save_guard_testing::clearFrame();pollNativeSaveGuardTransactions();}
void rejectBeforeCreate(const NativeSaveRequest& r){
    seed(r.targetPath,"ORIGINAL_KEEP");const auto before=bytes(r.targetPath);
    setNativeSaveGuardHasLiveARA(true);check(save_guard_testing::setFrame(r),"synthetic own frame rejected");
    const int fd=save_guard_testing::open(r.actualOpenPath.c_str(),O_RDWR|O_CREAT|O_TRUNC);
    check(fd<0,"protected unbound archive allowed the original temporary open");if(fd>=0)close(fd);
    check(!exists(r.actualOpenPath),"veto created a temporary output");
    check(save_guard_testing::unlink(r.targetPath.c_str())<0,"veto allowed old-target unlink");
    check(bytes(r.targetPath)==before,"unbound veto changed the previous target");finishFrame();
}
void ordinaryAfterNativeLifetimeEnded(const NativeSaveRequest& r){
    seed(r.targetPath,"OLD_ORDINARY");setNativeSaveGuardHasLiveARA(true);
    const auto preflights=bridge::runtime().nativeSavePreflights;check(save_guard_testing::setFrame(r),"ordinary own frame rejected");
    const int fd=save_guard_testing::open(r.actualOpenPath.c_str(),O_RDWR|O_CREAT|O_TRUNC);
    check(fd>=0,"terminated/released unbound protection never cleared");
    if(fd>=0){const std::string payload="ORDINARY_AFTER_LIFETIME_END";check(write(fd,payload.data(),payload.size())==(ssize_t)payload.size(),"own ordinary payload failed");close(fd);}
    check(bridge::runtime().nativeSavePreflights==preflights+1,"ordinary control bypassed the actual glue preflight");
    check(save_guard_testing::unlink(r.targetPath.c_str())==0,"ordinary precommit refused without any live ARA models");
    NSError* error=nil;check([[NSFileManager defaultManager]moveItemAtPath:@(r.actualOpenPath.c_str())toPath:@(r.targetPath.c_str())error:&error],"ordinary native Foundation commit failed");
    check(bytes(r.targetPath)=="ORDINARY_AFTER_LIFETIME_END","ordinary committed output differs");finishFrame();
}
void nativeStateReentryMustVeto(const NativeSaveRequest& r,bool offMainGetter,bool nullStream=false){
    seed(r.targetPath,"STATE_REENTRY_KEEP");const auto before=bytes(r.targetPath);setNativeSaveGuardHasLiveARA(true);
    check(save_guard_testing::setFrame(r),"state reentry frame rejected");
    const int fd=save_guard_testing::open(r.actualOpenPath.c_str(),O_RDWR|O_CREAT|O_TRUNC);
    check(fd>=0,"state reentry fixture failed before the tested state call");if(fd>=0)close(fd);
    native_hooks::InstanceRecord native;auto* stream=new rysup::ara_lab::fl_app::MemoryIBStream({1,2,3});
    const auto calls=ordinaryStateCalls.load();
    auto invoke=[&]{native_hooks::ThreadInfo info;info.isMainThread=!offMainGetter;
        native_hooks::OriginalStateCall original(&native,ordinaryState);
        const auto operation=offMainGetter?native_hooks::StateOperation::ComponentGetState:native_hooks::StateOperation::ControllerSetComponentState;
        const auto result=bridge::state(native,operation,nullStream?nullptr:stream,original,info,nullptr);
        check(result==((offMainGetter&&!nullStream)?Steinberg::kResultOk:Steinberg::kResultFalse),"state reentry forwarding/refusal policy differs");};
    if(offMainGetter){std::thread worker(invoke);worker.join();}else invoke();stream->release();
    check(ordinaryStateCalls==calls+((offMainGetter&&!nullStream)?1:0),"native state original was not called exactly as required");
    check(save_guard_testing::unlink(r.targetPath.c_str())<0,"state reentry failed to veto old-target unlink");
    NSError* error=nil;check(![[NSFileManager defaultManager]moveItemAtPath:@(r.actualOpenPath.c_str())toPath:@(r.targetPath.c_str())error:&error],"state reentry committed through Foundation");
    check(error&&error.code==EBUSY&&bytes(r.targetPath)==before&&exists(r.actualOpenPath),"state reentry lost target or temporary evidence");finishFrame();
}
void lateProtectedRecordBeforeCommit(const NativeSaveRequest& r,native_hooks::InstanceRecord& native){
    forceOwnerGuard=false;native.nativeAlive=false;native.ownerStateFlags=0;
    seed(r.targetPath,"LATE_RECORD_OLD_KEEP");const auto before=bytes(r.targetPath);const auto preflights=bridge::runtime().nativeSavePreflights;
    check(!bridge::hasLiveNativeARA(nullptr)&&save_guard_testing::setFrame(r),"late record fixture was not initially ordinary");
    const int fd=save_guard_testing::open(r.actualOpenPath.c_str(),O_RDWR|O_CREAT|O_TRUNC);
    check(fd>=0&&bridge::runtime().nativeSavePreflights==preflights&&!currentNativeSaveTransaction(),"ordinary first open unexpectedly minted protected save authority");if(fd>=0)close(fd);
    std::thread publication([&]{native.nativeAlive=true;bridge::preserveUnboundARA(native);});publication.join();
    check(bridge::hasLiveNativeARA(nullptr)&&!(native.ownerStateFlags.load()&bridge::nativeObservedStateDirtyPending),"background state publication lost fresh liveness or falsely dirtied an ordinary nontransaction load");
    check(save_guard_testing::unlink(r.targetPath.c_str())<0,"late protected record without admitted transaction unlinked old target");
    NSError* error=nil;check(![[NSFileManager defaultManager]moveItemAtPath:@(r.actualOpenPath.c_str())toPath:@(r.targetPath.c_str())error:&error],"late protected record committed without an admitted transaction");
    check(error&&error.code==EBUSY&&bytes(r.targetPath)==before&&exists(r.actualOpenPath),"late protected record lost target or temporary evidence");
    finishFrame();native.nativeAlive=false;forceOwnerGuard=true;
}
void nativeAdapterCacheQueueControls(){
    bridge::BridgeRecord r;native_hooks::InstanceRecord native;r.native=&native;native.nativeInitialized=true;native.ownerData=&r;
    r.identity.instanceID="native-adapter-cache-queue";r.nativeCacheGate=std::make_shared<rysup::ara_lab::StandaloneCacheGate>();
    auto* previous=bridge::runtime().head.exchange(&r);struct Restore{bridge::BridgeRecord* old;~Restore(){bridge::runtime().head=old;}}restore{previous};
    bridge::Adapter owner;rysup::ara_lab::StandaloneARAEditRelay relay;std::atomic<unsigned> requests{0};unsigned builds=0;std::uint64_t revision=0;
    const auto close=[&](std::uint64_t epoch)noexcept{owner.invalidateActualNativeOverlayImmediately(r.identity.instanceID,epoch);};
    const auto stop=[&](std::uint64_t epoch)noexcept{owner.queueActualNativeStopForCacheRebuild(r.identity.instanceID,epoch);};
    const auto notify=[&](std::uint64_t value)noexcept{relay.forwardActualRevision(value,[&](std::uint64_t actual)noexcept{owner.queueActualNativeDocumentDirtyFromARA(r.identity.instanceID,actual);});};
    const auto queue=[&]()noexcept{++requests;owner.enqueueModelThreadRebuild(r.identity.instanceID);};
    const auto edit=[&](std::uint64_t value)noexcept{rysup::ara_lab::invalidateStandaloneCacheForEdit(*r.nativeCacheGate,value,close,stop,notify,queue);};
    const auto concurrentEdit=[&]{const auto value=++revision;std::thread producer([&]{edit(value);});producer.join();};
    const auto attempt=[&](int phase){if(!r.dirty.exchange(false))return false;++builds;
        if(phase==0)concurrentEdit();
        const auto ticket=rysup::ara_lab::invalidateStandaloneCacheAuthority(*r.nativeCacheGate,close,stop);
        if(phase==1)concurrentEdit();const bool published=r.nativeCacheGate->publishIfUnchanged(ticket);
        if(phase==2)concurrentEdit();if(!published||!r.nativeCacheGate->authorizes(ticket))r.dirty=true;
        return published&&r.nativeCacheGate->authorizes(ticket);};
    check(attempt(-1)&&builds==1&&!r.dirty,"initial authority-only build requeued itself");
    check(requests==0&&r.nativeStopForRebuildPending&&r.nativeDirtyPendingRevision==0,"internal maintenance lost STOP authority or pretended to be an edit");
    const auto initial=r.nativeCacheGate->currentTicket();for(unsigned i=0;i<100;++i)check(!attempt(-1),"unchanged idle tick rebuilt current cache");
    check(builds==1&&r.nativeCacheGate->authorizes(initial),"quiet ticks changed current cache epoch");
    edit(++revision);check(r.dirty&&!r.nativeCacheGate->authorizes(initial),"real edit did not close authority/queue rebuild");
    check(attempt(-1)&&builds==2&&!r.dirty&&requests==1,"real edit did not produce exactly one new build");
    edit(0);check(r.dirty&&requests==2&&r.nativeDirtyPendingRevision==revision,"manual zero-revision request was ignored or faked an actual ARA revision");
    check(attempt(-1)&&!r.dirty,"manual model change failed to settle");
    for(int phase=0;phase<3;++phase){r.dirty=true;const auto before=builds;attempt(phase);
        check(r.dirty,"real edit concurrent with current build was lost");
        check(attempt(-1)&&builds==before+2&&!r.dirty,"concurrent edit did not settle after exactly one follow-up build");
        const auto stable=r.nativeCacheGate->currentTicket();check(!attempt(-1)&&r.nativeCacheGate->authorizes(stable),"post-edit quiet tick changed authority");}
    check(rysup::fl_ara::forbiddenSDKCalls==0,"cache queue controls invoked native SDK/capture");
}
void nativeSlotPowerHoldIdentityControls(){
    bridge::BridgeRecord r;native_hooks::InstanceRecord native;r.native=&native;native.ownerData=&r;
    native.nativeInitialized=true;r.identity.instanceID="power-hold";r.identity.actualComponent=0xa000;
    native.actualComponent=reinterpret_cast<Steinberg::Vst::IComponent*>(0xa000);r.insert.effectWrapper=0xb000;
    slot_power::Binding binding(0xb000,0xa000,1),replacement(0xb000,0xa000,2);r.nativeSlotPower=&binding;
    r.nativeCacheGate=std::make_shared<rysup::ara_lab::StandaloneCacheGate>();
    const auto ticket=r.nativeCacheGate->currentTicket();check(r.nativeCacheGate->publishIfUnchanged(ticket),"power fixture cache publication failed");r.dirty=false;
    rysup::ara_lab::FLStandaloneOverlayStaleHoldRecord hold;hold.instanceID=r.identity.instanceID;hold.actualNativeComponent=0xa000;
    hold.reason=rysup::ara_lab::FLStandaloneOverlayHoldReason::NativeSlotPowerUnknown;hold.nativeSlotPowerBinding=&binding;
    hold.nativeSlotPowerWrapper=0xb000;hold.nativeSlotPowerGeneration=1;
    check(bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"current Unknown power request was discarded");
    auto changed=hold;changed.reason=rysup::ara_lab::FLStandaloneOverlayHoldReason::StaleCache;
    check(!bridge::nativeSlotPowerHoldMatchesRecord(r,changed),"cache hold became power STOP");
    changed=hold;changed.instanceID="another";check(!bridge::nativeSlotPowerHoldMatchesRecord(r,changed),"another insert power hold was accepted");
    changed=hold;changed.actualNativeComponent=0xa001;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,changed),"another component power hold was accepted");
    changed=hold;changed.nativeSlotPowerGeneration=2;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,changed),"old pointer/new generation was accepted");
    changed=hold;changed.nativeSlotPowerWrapper=0xb001;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,changed),"another wrapper power hold was accepted");
    changed=hold;changed.nativeSlotPowerBindingMissing=true;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,changed),"missing identity accepted nonnull binding");
    r.nativeSlotPower=&replacement;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"replaced generation retained old STOP request");r.nativeSlotPower=&binding;
    r.teardown=true;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"teardown retained power STOP");r.teardown=false;
    r.alive=false;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"ended record retained power STOP");r.alive=true;
    native.nativeTerminated=true;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"terminated component retained power STOP");native.nativeTerminated=false;
    native.ownerData=nullptr;check(!bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"changed component owner retained power STOP");native.ownerData=&r;
    {auto notification=binding.beforeOriginal(0);notification.originalReturned();}
    check(!bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"completed Disabled notification retained stale STOP");
    {auto notification=binding.beforeOriginal(1);notification.originalReturned();}
    check(!bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"completed Enabled notification retained stale STOP");
    binding.revokeToUnknown();check(bridge::nativeSlotPowerHoldMatchesRecord(r,hold),"fresh Unknown state lost current identity");
    r.nativeSlotPowerStopPending=hold;bridge::drainNativeSlotPowerStop(r);
    check(!r.nativeSlotPowerStopPending,"absent overlay retained a stale power STOP");
    check(!r.dirty&&!r.nativeStopForRebuildPending&&r.nativeCacheGate->authorizes(ticket),"power drain changed coherent ARA cache authority");
    check(rysup::fl_ara::forbiddenSDKCalls==0,"invalid/stale power drain called native STOP or capture");
}
struct EchoQueue final:Steinberg::Vst::IParamValueQueue {
    Steinberg::Vst::ParamID id=7;int points=1,offset=0;double value=.25;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID,void** out)override{if(out)*out=nullptr;return Steinberg::kNoInterface;}
    Steinberg::uint32 PLUGIN_API addRef()override{return 1;}Steinberg::uint32 PLUGIN_API release()override{return 1;}
    Steinberg::Vst::ParamID PLUGIN_API getParameterId()override{return id;}
    Steinberg::int32 PLUGIN_API getPointCount()override{return points;}
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32,Steinberg::int32& o,double& v)override{o=offset;v=value;return Steinberg::kResultOk;}
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32,double,Steinberg::int32&)override{return Steinberg::kNotImplemented;}
};
struct EchoChanges final:Steinberg::Vst::IParameterChanges {
    EchoQueue queue;int count=1;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID,void** out)override{if(out)*out=nullptr;return Steinberg::kNoInterface;}
    Steinberg::uint32 PLUGIN_API addRef()override{return 1;}Steinberg::uint32 PLUGIN_API release()override{return 1;}
    Steinberg::int32 PLUGIN_API getParameterCount()override{return count;}
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32)override{return &queue;}
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID&,Steinberg::int32&)override{return nullptr;}
};
void exactParameterEchoControls(){
    bridge::BridgeRecord r;native_hooks::InstanceRecord n,controller;r.native=&n;n.ownerData=&r;
    r.identity.actualComponent=0xa000;n.actualComponent=reinterpret_cast<Steinberg::Vst::IComponent*>(0xa000);
    bridge::ObservedNativeInstance node;node.native=&controller;node.controllerOwner=&r;
    node.verifiedController=reinterpret_cast<Steinberg::Vst::IEditController*>(0xb000);controller.actualController=node.verifiedController.load();r.currentControllerNode=&node;
    r.nativeCacheGate=std::make_shared<rysup::ara_lab::StandaloneCacheGate>();const auto ticket=r.nativeCacheGate->currentTicket();check(r.nativeCacheGate->publishIfUnchanged(ticket),"echo cache fixture failed");r.dirty=false;
    auto proof=std::make_unique<bridge::NativeParameterEchoSnapshot>();proof->actualComponent=0xa000;proof->controllerNode=&node;proof->controller=node.verifiedController.load();proof->ticket=ticket;
    proof->parameters.valid=true;proof->parameters.count=1;proof->parameters.rows[0]={7,.25,Steinberg::Vst::ParameterInfo::kCanAutomate};
    r.parameterEchoStorage=std::move(proof);r.parameterEchoSnapshot=r.parameterEchoStorage.get();EchoChanges changes;native_hooks::ThreadInfo info;info.insideNativeProcess=true;
    check(bridge::exactCurrentCacheParameterEcho(r,&changes,1,128),"exact current-cache offset0 echo rejected");
    bridge::inputParameterChanges(n,&changes,1,128,info,nullptr);check(r.nativeInputQueueExactEchoes==1&&!r.dirty&&r.nativeCacheGate->authorizes(ticket),"echo invalidated exact current cache");
    changes.queue.value=.5;check(!bridge::exactCurrentCacheParameterEcho(r,&changes,1,128),"real changed value was suppressed");changes.queue.value=.25;
    changes.queue.offset=1;check(!bridge::exactCurrentCacheParameterEcho(r,&changes,1,128),"sample automation was suppressed");changes.queue.offset=0;
    changes.queue.points=2;check(!bridge::exactCurrentCacheParameterEcho(r,&changes,1,128),"multi-point automation was suppressed");changes.queue.points=1;
    changes.queue.id=8;check(!bridge::exactCurrentCacheParameterEcho(r,&changes,1,128),"unknown parameter was suppressed");changes.queue.id=7;
    changes.count=2;check(!bridge::exactCurrentCacheParameterEcho(r,&changes,2,128),"duplicate parameter queue was suppressed");changes.count=1;
    check(!bridge::exactCurrentCacheParameterEcho(r,nullptr,1,128)&&!bridge::exactCurrentCacheParameterEcho(r,&changes,-1,128),"malformed queue was suppressed");
    node.controllerOwner=nullptr;check(!bridge::exactCurrentCacheParameterEcho(r,&changes,1,128),"replaced controller owner retained echo authority");node.controllerOwner=&r;
    r.nativeCacheGate->invalidate();check(!bridge::exactCurrentCacheParameterEcho(r,&changes,1,128),"stale snapshot retained echo authority");
    bridge::inputParameterChanges(n,&changes,1,128,info,nullptr);check(r.dirty&&r.nativeStopForRebuildPending,"stale echo did not request normal invalidation");
    check(rysup::fl_ara::forbiddenSDKCalls==0,"echo RT controls invoked native capture/controller work");
}
void physicalIdentityDiagnosticJSONControls(){
    rysup::ara_lab::FLStandaloneOverlayInstanceStats cache;cache.physicalIdentityDiagnosticReadyMask=0b0101;cache.physicalIdentityDiagnosticOverflow=true;
    for(unsigned i=0;i<cache.physicalIdentityDiagnostics.size();++i){auto& d=cache.physicalIdentityDiagnostics[i];d.slotIndex=i;
        d.branch=rysup::ara_lab::FLStandalonePhysicalIdentityBranch::ObservedPhysicalSourceB;d.mismatchBits=1u<<10;d.protectedTableAddress=0xdeadbeef;
        d.observed.nativeRate=std::numeric_limits<double>::quiet_NaN();d.observed.nativeRateFactor=std::numeric_limits<double>::infinity();
        d.observed.readerStep=-std::numeric_limits<double>::infinity();d.observed.sourceStartFrame=123.25;d.observed.preOriginalReaderPosition=0;
        d.observed.nativeInterleavedStereo=reinterpret_cast<float*>(0xbadf00d);d.observed.globalPreviewOwnerReadCoherent=true;d.observed.globalPreviewChannelObject=0xabc123;d.entry.nativeRecord[87]=91;d.entry.expected.managerMemberPresent=true;}
    NSArray* history=bridge::physicalIdentityHistoryDiagnosticJSON(cache);check(history.count==2,"physical diagnostic JSON ignored ready mask/capacity");
    check([history[0][@"slot_index"]unsignedIntValue]==0&&[history[1][@"slot_index"]unsignedIntValue]==2,"physical diagnostic JSON included an unpublished slot");
    check([history[0][@"branch"]unsignedIntValue]==2&&[history[0][@"mismatch_bits"]unsignedIntValue]==(1u<<10),"physical diagnostic branch/mismatch changed");
    check([history[0][@"protected_table_address"]unsignedLongLongValue]==0xdeadbeef,"physical diagnostic address was changed or dereferenced");
    NSDictionary* observed=history[0][@"observed"];
    for(NSString* key in @[@"native_rate",@"native_rate_factor",@"reader_step"]){check(![observed[key][@"finite"]boolValue]&&observed[key][@"value"]==NSNull.null,"rejected nonfinite POD polluted JSON numbers");}
    check([observed[@"source_start_frame"][@"value"]doubleValue]==123.25&&[observed[@"source_start_frame"][@"finite"]boolValue],"finite observed double changed");
    std::uint64_t bits=0;const auto raw=cache.physicalIdentityDiagnostics[0].observed.nativeRate;std::memcpy(&bits,&raw,sizeof(bits));
    check([observed[@"native_rate"][@"raw_bits"]unsignedLongLongValue]==bits,"physical diagnostic NaN payload bits lost");
    check([observed[@"native_interleaved_stereo"]unsignedLongLongValue]==0xbadf00d,"observed output pointer was dereferenced/changed");
    check([observed[@"global_preview_owner_read_coherent"]boolValue]&&[observed[@"global_preview_channel_object"]unsignedLongLongValue]==0xabc123,"preview owner POD diagnostic was omitted or dereferenced");
    check([history[0][@"entry"][@"native_record"]count]==88&&[history[0][@"entry"][@"native_record"][87]unsignedIntValue]==91,"physical record88 diagnostic was truncated");
    check([history[0][@"entry"][@"expected"][@"manager_member_present"]boolValue],"prepared expected model diagnostic absent");
    NSError* error=nil;NSData* json=[NSJSONSerialization dataWithJSONObject:history options:0 error:&error];
    check(json&& !error,"malformed rejected POD prevented entire JSON status");
    check(rysup::fl_ara::forbiddenSDKCalls==0,"physical JSON diagnostics entered native/SDK work");
}
void nativeSetupDiagnosticControls(const std::string& root){
    auto owned=std::make_unique<bridge::BridgeRecord>();auto& r=*owned;native_hooks::InstanceRecord n;r.native=&n;n.ownerData=&r;
    r.identity.instanceID="setup-diagnostic-control";r.nativeCacheGate=std::make_shared<rysup::ara_lab::StandaloneCacheGate>();r.dirty=false;
    native_hooks::ThreadInfo info;info.isMainThread=false;
    Steinberg::Vst::ProcessSetup data{};data.processMode=Steinberg::Vst::kRealtime;data.symbolicSampleSize=Steinberg::Vst::kSample32;
    data.maxSamplesPerBlock=256;data.sampleRate=48000;const auto epoch=r.nativeCacheGate->currentTicket().epoch;
    bridge::setup(n,data,Steinberg::kResultFalse,info,nullptr);
    const auto& first=r.nativeSetupHistory[0];check(first.ready.load()&&first.sequence==1,"setup completion was not published");
    check(first.actualSetup.sampleRate==48000&&first.actualSetup.maxSamplesPerBlock==256&&first.originalResult==Steinberg::kResultFalse,"setup exact POD/result changed in history");
    check(first.epochBefore==epoch&&first.epochAfter==epoch+1&&!first.dirtyBefore&&first.dirtyAfter,"setup diagnostics altered/missed existing invalidation behavior");
    check(!first.qualifiedBefore&&!first.qualifiedAfter&&!first.mainThread,"failed setup qualification/thread history is wrong");
    data.processMode=Steinberg::Vst::kOffline;data.maxSamplesPerBlock=1024;
    bridge::setup(n,data,Steinberg::kResultOk,info,nullptr);
    check(r.setupQualified&&r.nativeSetupHistory[1].qualifiedAfter&&r.nativeSetupHistory[1].actualSetup.processMode==Steinberg::Vst::kOffline,"successful off-main setup evidence was not retained");
    data.sampleRate=std::numeric_limits<double>::quiet_NaN();data.maxSamplesPerBlock=-1;
    bridge::setup(n,data,Steinberg::kResultFalse,info,nullptr);
    check(!std::isfinite(r.nativeSetupHistory[2].actualSetup.sampleRate)&&r.nativeSetupHistory[2].actualSetup.maxSamplesPerBlock==-1,"nonfinite diagnostic preimage was silently changed");
    std::array<std::thread,40> writers;std::atomic<bool> begin{false};
    for(unsigned i=0;i<writers.size();++i)writers[i]=std::thread([&,i]{while(!begin.load())std::this_thread::yield();
        Steinberg::Vst::ProcessSetup actual{};actual.processMode=static_cast<Steinberg::int32>(100+i);actual.symbolicSampleSize=Steinberg::Vst::kSample32;actual.maxSamplesPerBlock=512;actual.sampleRate=48000;
        bridge::setup(n,actual,Steinberg::kResultFalse,info,nullptr);});
    begin=true;for(auto& writer:writers)writer.join();
    check(r.nativeSetupDiagnosticCount==43,"bounded setup history lost total callback count");
    for(unsigned i=0;i<r.nativeSetupHistory.size();++i){const auto& event=r.nativeSetupHistory[i];
        check(event.ready.load(std::memory_order_acquire)&&event.sequence==i+1,"concurrent setup completion slot missing/reused");
        check(event.epochAfter>event.epochBefore,"setup history observed no unchanged production invalidation");}
    check(first.actualSetup.maxSamplesPerBlock==256&&first.epochBefore==epoch,"overflow overwrote first retained setup evidence");
    auto& rt=bridge::runtime();auto& app=rysup::fl_ara::testApplication;const auto oldDirectory=app.runDirectory;app.runDirectory=root;
    rt.records.push_back(std::move(owned));check(bridge::status(),"setup diagnostic status write failed");
    NSData* bytes=[NSData dataWithContentsOfFile:@((root+"/fl-ara-status.json").c_str())];NSDictionary* status=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
    NSDictionary* insert=[status[@"inserts"]firstObject];NSArray* history=insert[@"native_setup_history"];
    check([insert[@"native_setup_diagnostic_count"]unsignedLongLongValue]==43&&history.count==32&&[insert[@"native_setup_history_overflow"]unsignedLongLongValue]==11,"setup status omitted bounded count/overflow");
    check([insert[@"native_setup_history_ready_mask"]unsignedLongLongValue]==0xffffffffULL,"setup status ready mask disagrees with the same completed entries");
    check([status[@"source_overlay_global_counters"][@"qualified_preview_unrelated_callbacks"]unsignedLongLongValue]==0&&status[@"source_overlay_global_counters"]!=nil,"global preview counter status is missing or falsely attributed to an insert");
    check(![history[2][@"sample_rate_finite"]boolValue]&&[history[2][@"sample_rate"]doubleValue]==0.0,"nonfinite setup value made JSON invalid or pretended finite");
    check([history[1][@"process_mode"]intValue]==Steinberg::Vst::kOffline&&[history[1][@"original_result"]intValue]==Steinberg::kResultOk,"status changed offline ProcessSetup/result");
    r.nativeSetupDiagnosticCount=std::numeric_limits<std::uint64_t>::max()-1;
    data.sampleRate=48000;for(unsigned i=0;i<3;++i)bridge::setup(n,data,Steinberg::kResultFalse,info,nullptr);
    check(r.nativeSetupDiagnosticCount==std::numeric_limits<std::uint64_t>::max()&&r.nativeSetupDiagnosticSlots==32,"near-wrap count/reservation did not saturate independently");
    check(first.sequence==1&&first.actualSetup.maxSamplesPerBlock==256&&first.epochBefore==epoch,"near-wrap setup callback reused immutable diagnostic slot");
    rt.records.clear();n.ownerData=nullptr;app.runDirectory=oldDirectory;
    check(rysup::fl_ara::forbiddenSDKCalls==0,"setup diagnostic controls entered native/SDK work");
}
void nativeReadyStatusPublication(const std::string& root){
    auto& rt=bridge::runtime();auto& app=rysup::fl_ara::testApplication;app.runDirectory=root;app.admitted=true;app.engineReady=false;
    rt.unboundArchiveRefusals=0;rt.unsupportedOffMainARAInitializations=0;rt.nativeSaveAlertQueued=false;rt.nativeStopNoticeQueued=false;rt.nativeReadyStatusPublished=false;
    check(bridge::status(),"initial own status write failed");const auto file=root+"/fl-ara-status.json";const auto original=bytes(file);
    app.engineReady=true;std::thread background([&]{bridge::tick(app,nullptr);});background.join();
    check(!rt.nativeReadyStatusPublished&&bytes(file)==original,"off-main tick published native readiness");
    rt.inTick=true;bridge::tick(app,nullptr);rt.inTick=false;
    check(!rt.nativeReadyStatusPublished&&bytes(file)==original,"nested tick published native readiness");
    const auto r=request(root,"status-during-save",0x3d00);check(save_guard_testing::setFrame(r),"status save fixture rejected");
    const int fd=save_guard_testing::open(r.actualOpenPath.c_str(),O_RDWR|O_CREAT|O_TRUNC);check(fd>=0,"status save fixture did not open");if(fd>=0)close(fd);
    bridge::tick(app,nullptr);check(!rt.nativeReadyStatusPublished&&bytes(file)==original,"foreign save tick published status or entered model work");finishFrame();
    bridge::tick(app,nullptr);
    check(rt.nativeReadyStatusPublished,"first eligible tick failed to publish before later model refusal");
    NSData* data=[NSData dataWithContentsOfFile:@(file.c_str())];NSDictionary* status=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    check([status[@"native_engine_ready"]boolValue]&&![status[@"runtime_qualified"]boolValue]&&[status[@"inserts"]count]==0,"early status misrepresented actual graph/qualification");
    check(rysup::fl_ara::forbiddenSDKCalls==0,"initial readiness control performed native SDK/capture");
    app=NativeApplication{};rt.nativeReadyStatusPublished=false;rt.error.clear();
}
void lateOwnerVeto(const NativeSaveRequest& r,const std::string& expected){
    seed(r.targetPath,"OLD_BOUND_ARC_KEEP");const auto before=bytes(r.targetPath);
    setNativeSaveGuardHasLiveARA(true);check(save_guard_testing::setFrame(r),"late own frame rejected");
    const int fd=save_guard_testing::open(r.actualOpenPath.c_str(),O_RDWR|O_CREAT|O_TRUNC);
    check(fd>=0,"negative fixture did not reach its intended post-open precommit gate");
    if(fd>=0){const std::string partial="INCOMPLETE_NATIVE_WRITER";check(write(fd,partial.data(),partial.size())==(ssize_t)partial.size(),"own partial payload failed");close(fd);}
    check(save_guard_testing::unlink(r.targetPath.c_str())<0,"incomplete all-component freeze unlinked the old target");
    check(bridge::runtime().nativeSaveError.find(expected)!=std::string::npos,"owner veto arose from a different precommit check");
    NSError* error=nil;check(![[NSFileManager defaultManager]moveItemAtPath:@(r.actualOpenPath.c_str())toPath:@(r.targetPath.c_str())error:&error],"incomplete freeze committed through Foundation");
    check(error&&error.code==EBUSY,"Foundation veto did not come from the native save guard");
    check(bytes(r.targetPath)==before&&exists(r.actualOpenPath),"late veto changed target or removed own temporary evidence");finishFrame();
}
}
int main(int argc,char** argv){@autoreleasepool{try{
    check(argc==2,"pass an existing own build directory");std::string pattern=std::string(argv[1])+"/own-files-XXXXXX";
    std::vector<char> rootBytes(pattern.begin(),pattern.end());rootBytes.push_back(0);check(mkdtemp(rootBytes.data())!=nullptr,"own test root creation failed");
    const std::string root=rootBytes.data();
    NativeSaveGuardCallbacks callbacks;callbacks.preflight=ownerPreflight;callbacks.precommit=bridge::nativeSavePrecommit;
    callbacks.rejected=bridge::nativeSaveRejected;callbacks.finished=bridge::nativeSaveFinished;
    // Force owner preflight even with no live ARA in explicit ordinary-lifetime
    // controls; production uses bridge::hasLiveNativeARA directly.
    callbacks.hasLiveARA=[](void*)noexcept{return forceOwnerGuard.load()||bridge::hasLiveNativeARA(nullptr);};
    check(save_guard_testing::initialize(callbacks,root),"test-only save guard initialization failed");
    native_hooks::InstanceRecord unbound;unbound.identity=41;
    bridge::ObservedNativeInstance observed;observed.native=&unbound;bridge::runtime().observedNativeInstances=&observed;
    recognizedStateMustRemainProtected(unbound,native_hooks::StateOperation::ComponentSetState);rejectBeforeCreate(request(root,"unbound",0x1000));
    check(bridge::hasLiveProtectedNativeArchive()&&bridge::hasLiveNativeARA(nullptr),"alive unbound archive was not protected by fresh predicate");
    unbound.nativeTerminated=true;ordinaryAfterNativeLifetimeEnded(request(root,"terminated",0x2000));
    check(!bridge::hasLiveProtectedNativeArchive()&&!bridge::hasLiveNativeARA(nullptr),"actual hook termination flag did not end fresh unbound protection");
    unbound.nativeTerminated=false;unbound.nativeAlive=false;ordinaryAfterNativeLifetimeEnded(request(root,"released",0x3000));
    check(!bridge::hasLiveProtectedNativeArchive()&&!bridge::hasLiveNativeARA(nullptr),"actual final-release flag did not end fresh unbound protection");
    unbound.nativeAlive=true;unbound.ownerStateFlags=0;
    recognizedStateMustRemainProtected(unbound,native_hooks::StateOperation::ControllerSetComponentState);
    rejectBeforeCreate(request(root,"unbound-controller",0x3800));unbound.nativeAlive=false;
    nativeStateReentryMustVeto(request(root,"off-main-state",0x3900),true);
    nativeStateReentryMustVeto(request(root,"controller-save-reentry",0x3a00),false);
    nativeStateReentryMustVeto(request(root,"null-state",0x3b00),false,true);
    lateProtectedRecordBeforeCommit(request(root,"late-protected-record",0x3c00),unbound);
    scopedParameterCallbacks();
    nativeAdapterCacheQueueControls();
    nativeSlotPowerHoldIdentityControls();
    exactParameterEchoControls();
    physicalIdentityDiagnosticJSONControls();
    nativeSetupDiagnosticControls(root);
    nativeReadyStatusPublication(root);
    for(unsigned i=0;i<2;++i){auto r=std::make_unique<bridge::BridgeRecord>();r->native=&negativeNativeRecords[i];r->initialPrimePending=false;
        bridge::runtime().records.push_back(std::move(r));}
    admission=Admission::MissingMember;lateOwnerVeto(request(root,"missing-member",0x4000),"membership changed");
    admission=Admission::MissingSecondSerialization;lateOwnerVeto(request(root,"missing-second-envelope",0x5000),"expected ARA envelope was not serialized");
    check(forbiddenSDKCalls==0,"rejection controls entered a native capture/SDK operation");
    for(auto& r:bridge::runtime().records){r->alive=false;r->native->nativeAlive=false;}
    bridge::runtime().records.clear();setNativeSaveGuardHasLiveARA(false);
    std::cout<<"{\"schema\":\"rysup.fl-native-save-glue-controls/1\",\"pass\":true,\"checks\":"<<checks
        <<",\"native_controller_association_stubbed\":true,\"own_file_root\":\""<<root<<"\",\"native_admission_stubbed\":true,\"opaque_archive_authority_forged\":false,\"DAW_or_plugin_loaded\":false,\"native_FLP_format_qualified\":false}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}}
