// Offline ABI/data-page controls. Includes private installer primitives for
// synthetic negative testing; never admits/loads/hooks vendor code or a DAW.
#include "FLNativeSlotPowerObserver.cpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>
#include <condition_variable>
static std::atomic<bool> countAllocations{false};
static std::atomic<std::uint64_t> allocations{0};
void* operator new(std::size_t bytes){if(countAllocations.load())++allocations;if(void* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete[](void* p)noexcept{::operator delete(p);}
using namespace rysup::fl_ara::slot_power;
static unsigned checks=0;
#define CHECK(...) do{++checks;assert((__VA_ARGS__));}while(false)
struct Fake {
    Registry<32>* registry=nullptr;Binding* binding=nullptr;Binding* replacement=nullptr;
    void* self=nullptr;std::intptr_t id=0,index=0,value=0;
    unsigned calls=0;bool expectUnknown=false,reenter=false,exception=false,replace=false;
    std::intptr_t result=INTPTR_MIN+91;
};
static Fake fake;
struct Concurrent {
    std::mutex mutex;std::condition_variable condition;unsigned arrived=0;
    bool firstMayReturn=false,secondMayReturn=false;Binding* binding=nullptr;
} concurrent;
static std::intptr_t concurrentOriginal(void*,std::intptr_t id,std::intptr_t index,std::intptr_t value){
    assert(id==setEnabledID&&(index==1||index==2)&&(value==0||value==1));
    std::unique_lock<std::mutex> lock(concurrent.mutex);++concurrent.arrived;concurrent.condition.notify_all();
    assert(concurrent.binding->snapshotForBlock()==Power::Unknown);
    concurrent.condition.wait(lock,[&]{return index==1?concurrent.firstMayReturn:concurrent.secondMayReturn;});return INTPTR_MAX-index;
}
static std::intptr_t original(void* self,std::intptr_t id,std::intptr_t index,std::intptr_t value){
    ++fake.calls;fake.self=self;fake.id=id;fake.index=index;fake.value=value;
    if(fake.expectUnknown)assert(fake.binding->snapshotForBlock()==Power::Unknown);
    if(fake.reenter){fake.reenter=false;const auto result=dispatcherThunk(self,setEnabledID,INTPTR_MIN,0);assert(result==fake.result);}
    if(fake.replace){fake.replace=false;fake.registry->retireAtModelBoundary(0);
        assert(fake.registry->publishAtModelBoundary(0,*fake.replacement));
        const auto seed=fake.replacement->captureInitialSeedAtStoppedBoundary();assert(fake.replacement->trySeedAtStoppedBoundary(seed,1));}
    if(fake.exception)throw std::runtime_error("fake original exception");
    return fake.result;
}
template<class T>static void put(std::vector<std::uint8_t>& bytes,std::size_t offset,const T& value){std::memcpy(bytes.data()+offset,&value,sizeof(value));}
static std::vector<std::uint8_t> syntheticImage(){
    std::vector<std::uint8_t> data(sizeof(mach_header_64)+sizeof(uuid_command)+2*sizeof(segment_command_64));
    mach_header_64 header{};header.magic=MH_MAGIC_64;header.cputype=CPU_TYPE_ARM64;header.cpusubtype=CPU_SUBTYPE_ARM64_ALL;
    header.filetype=MH_DYLIB;header.ncmds=3;header.sizeofcmds=static_cast<std::uint32_t>(data.size()-sizeof(header));put(data,0,header);
    uuid_command uuid{};uuid.cmd=LC_UUID;uuid.cmdsize=sizeof(uuid);uuid.uuid[0]=37;put(data,sizeof(header),uuid);
    segment_command_64 code{};code.cmd=LC_SEGMENT_64;code.cmdsize=sizeof(code);code.vmaddr=0;code.vmsize=0x100000;code.initprot=VM_PROT_READ|VM_PROT_EXECUTE;
    put(data,sizeof(header)+sizeof(uuid),code);
    auto storage=code;storage.vmaddr=0x200000;storage.vmsize=0x100000;storage.initprot=VM_PROT_READ;
    put(data,sizeof(header)+sizeof(uuid)+sizeof(code),storage);return data;
}
int main(int argc,char** argv){
    CHECK(sizeof(std::intptr_t)==8&&sizeof(Dispatcher)==8);
    Registry<32> registry;Binding binding(0x12345678,0x23456789,1),replacement(binding.wrapper,0x34567890,2);
    CHECK(registry.publishAtModelBoundary(0,binding));fake.registry=&registry;fake.binding=&binding;fake.replacement=&replacement;
    publication.registry.store(&registry);publication.original.store(&original);
    const auto self=reinterpret_cast<void*>(binding.wrapper);
    fake.expectUnknown=true;
    CHECK(dispatcherThunk(self,setEnabledID,INTPTR_MAX,1)==fake.result);
    CHECK(fake.calls==1&&fake.self==self&&fake.id==11&&fake.index==INTPTR_MAX&&fake.value==1&&binding.snapshotForBlock()==Power::Enabled);
    CHECK(dispatcherThunk(self,setEnabledID,INTPTR_MIN,0)==fake.result&&binding.snapshotForBlock()==Power::Disabled);
    CHECK(fake.calls==2&&fake.index==INTPTR_MIN&&fake.value==0);
    CHECK(dispatcherThunk(self,setEnabledID,-1,INTPTR_MAX)==fake.result&&binding.snapshotForBlock()==Power::Unknown);
    fake.expectUnknown=false;
    CHECK(dispatcherThunk(nullptr,INTPTR_MIN,INTPTR_MAX,INTPTR_MIN)==fake.result);
    CHECK(fake.self==nullptr&&fake.id==INTPTR_MIN&&fake.index==INTPTR_MAX&&fake.value==INTPTR_MIN&&fake.calls==4);
    const auto unrelated=reinterpret_cast<void*>(std::uintptr_t{0x87654321});
    CHECK(dispatcherThunk(unrelated,setEnabledID,-71,1)==fake.result&&fake.calls==5&&fake.self==unrelated&&binding.snapshotForBlock()==Power::Unknown);
    fake.expectUnknown=true;fake.reenter=true;const auto beforeReentry=fake.calls;
    CHECK(dispatcherThunk(self,setEnabledID,0,1)==fake.result&&fake.calls==beforeReentry+2&&binding.snapshotForBlock()==Power::Unknown);
    CHECK(binding.callsInFlight()==0&&nativeSlotPowerObserverStats().inFlight==0);
    CHECK(dispatcherThunk(self,setEnabledID,0,1)==fake.result&&binding.snapshotForBlock()==Power::Enabled);
    fake.exception=true;const auto beforeException=fake.calls;bool caught=false;
    try{dispatcherThunk(self,setEnabledID,0,0);}catch(const std::runtime_error&){caught=true;}
    fake.exception=false;CHECK(caught&&fake.calls==beforeException+1&&binding.snapshotForBlock()==Power::Unknown&&binding.callsInFlight()==0);
    CHECK(nativeSlotPowerObserverStats().inFlight==0);
    fake.replace=true;
    CHECK(dispatcherThunk(self,setEnabledID,0,0)==fake.result&&binding.closed());
    CHECK(replacement.snapshotForBlock()==Power::Enabled&&binding.snapshotForBlock()==Power::Unknown);
    // Late completion used ONLY the captured old Pending, not a second W lookup.
    fake.binding=&replacement;fake.expectUnknown=true;
    CHECK(dispatcherThunk(self,setEnabledID,0,0)==fake.result&&replacement.snapshotForBlock()==Power::Disabled);
    fake.expectUnknown=false;allocations=0;countAllocations=true;const auto beforeLoop=fake.calls;
    for(unsigned i=0;i<20000;++i)assert(dispatcherThunk(unrelated,-33,INTPTR_MIN,INTPTR_MAX)==fake.result);
    countAllocations=false;CHECK(allocations==0&&fake.calls==beforeLoop+20000);
    const auto stats=nativeSlotPowerObserverStats();CHECK(stats.entries==fake.calls&&stats.normalReturns==stats.entries-1&&stats.inFlight==0);
    // Two actual shim entries overlap inside their originals. Unknown spans
    // both originals and remains latched after either completion order.
    Binding parallel(0x98765432,0x13572468,1);CHECK(registry.publishAtModelBoundary(1,parallel));
    concurrent.binding=&parallel;publication.original.store(&concurrentOriginal);
    const auto parallelSelf=reinterpret_cast<void*>(parallel.wrapper);
    std::thread first([&]{assert(dispatcherThunk(parallelSelf,setEnabledID,1,1)==INTPTR_MAX-1);});
    std::thread second([&]{assert(dispatcherThunk(parallelSelf,setEnabledID,2,0)==INTPTR_MAX-2);});
    {std::unique_lock<std::mutex> lock(concurrent.mutex);concurrent.condition.wait(lock,[&]{return concurrent.arrived==2;});}
    CHECK(parallel.callsInFlight()==2&&parallel.snapshotForBlock()==Power::Unknown&&nativeSlotPowerObserverStats().inFlight==2);
    {std::lock_guard<std::mutex> lock(concurrent.mutex);concurrent.firstMayReturn=true;concurrent.condition.notify_all();}
    first.join();CHECK(parallel.callsInFlight()==1&&parallel.snapshotForBlock()==Power::Unknown);
    {std::lock_guard<std::mutex> lock(concurrent.mutex);concurrent.secondMayReturn=true;concurrent.condition.notify_all();}
    second.join();CHECK(parallel.callsInFlight()==0&&parallel.snapshotForBlock()==Power::Unknown&&nativeSlotPowerObserverStats().inFlight==0);
    publication.original.store(&original);
    CHECK(dispatcherThunk(parallelSelf,setEnabledID,0,1)==fake.result&&parallel.snapshotForBlock()==Power::Enabled);
    // Real kernel VM controls apply ONLY to this test's new anonymous data page.
    mach_vm_address_t memory=0;CHECK(mach_vm_allocate(mach_task_self(),&memory,vm_page_size,VM_FLAGS_ANYWHERE)==KERN_SUCCESS);
    const auto slot=static_cast<std::uintptr_t>(memory);const auto a=reinterpret_cast<std::uintptr_t>(&original),b=reinterpret_cast<std::uintptr_t>(&dispatcherThunk);
    std::memcpy(reinterpret_cast<void*>(slot),&a,sizeof(a));
    CHECK(mach_vm_protect(mach_task_self(),memory,vm_page_size,false,VM_PROT_READ)==KERN_SUCCESS);
    const auto change=replaceDataPointer(slot,a,b);std::uintptr_t current=0;Region page;
    CHECK(change.changed&&change.restored&&read(slot,&current,sizeof(current))&&current==b);
    CHECK(regionAt(slot,sizeof(current),page)&&page.protection==VM_PROT_READ);
    const auto mismatch=replaceDataPointer(slot,a,a);CHECK(!mismatch.changed&&mismatch.restored&&read(slot,&current,sizeof(current))&&current==b);
    CHECK(!replaceDataPointer(slot+1,b,a).changed);
    CHECK(!replaceDataPointer(UINTPTR_MAX-3,b,a).changed&&!replaceDataPointer(0,b,a).changed);
    CHECK(!replaceDataPointer(slot,b,0).changed);
    CHECK(mach_vm_protect(mach_task_self(),memory,vm_page_size,false,VM_PROT_READ|VM_PROT_EXECUTE)==KERN_SUCCESS);
    CHECK(!replaceDataPointer(slot,b,a).changed);
    CHECK(mach_vm_deallocate(mach_task_self(),memory,vm_page_size)==KERN_SUCCESS);
    CHECK(!read(0,&current,sizeof(current))&&!read(UINTPTR_MAX-3,&current,sizeof(current)));
    // Actual helper provenance/no-load pinning, never native Wrapper loading.
    Dl_info own{};CHECK(dladdr(reinterpret_cast<void*>(&original),&own)&&own.dli_fbase&&own.dli_fname);
    CHECK(exactImageAt(a,reinterpret_cast<std::uintptr_t>(own.dli_fbase),canonical(own.dli_fname)));
    CHECK(!exactImageAt(a,reinterpret_cast<std::uintptr_t>(own.dli_fbase)+1,canonical(own.dli_fname)));
    CHECK(!exactImageAt(a,reinterpret_cast<std::uintptr_t>(own.dli_fbase),"/not/this/image"));
    CHECK(retainLoaded(a)!=nullptr&&retainLoaded(1)==nullptr);
    CHECK(!instructionPin(a,UINTPTR_MAX,prologueSHA));
    auto data=syntheticImage();Image image;CHECK(parseImage(data,image)&&image.uuid[0]==37&&image.plainARM64&&image.slotData&&image.dispatcherExecutable);
    auto bad=data;auto header=at<mach_header_64>(bad.data(),0);header.cpusubtype=CPU_SUBTYPE_ARM64E;put(bad,0,header);Image arm64e;CHECK(!parseImage(bad,arm64e));
    bad=data;header=at<mach_header_64>(bad.data(),0);header.cputype=CPU_TYPE_X86_64;put(bad,0,header);Image x86;CHECK(!parseImage(bad,x86));
    bad=data;header=at<mach_header_64>(bad.data(),0);header.ncmds=4097;put(bad,0,header);Image many;CHECK(!parseImage(bad,many));
    bad=data;auto command=at<load_command>(bad.data(),sizeof(header));command.cmdsize=7;put(bad,sizeof(header),command);Image malformed;CHECK(!parseImage(bad,malformed));
    bad=data;auto segment=at<segment_command_64>(bad.data(),sizeof(header)+sizeof(uuid_command)+sizeof(segment_command_64));segment.initprot|=VM_PROT_EXECUTE;put(bad,sizeof(header)+sizeof(uuid_command)+sizeof(segment_command_64),segment);Image executableSlot;CHECK(!parseImage(bad,executableSlot));
    bad=data;segment=at<segment_command_64>(bad.data(),sizeof(header)+sizeof(uuid_command));segment.initprot=VM_PROT_READ;put(bad,sizeof(header)+sizeof(uuid_command),segment);Image noExecutable;CHECK(!parseImage(bad,noExecutable));
    ObserverConfig config;CHECK(installLoadedNativeSlotPowerObserver(config).reason==ObserverInstallReason::Configuration);
    config.registry=&registry;config.actualWrapperImageBase=1;config.confirmAdmittedModelBoundary=[](void*)noexcept{return false;};
    CHECK(installLoadedNativeSlotPowerObserver(config).reason==ObserverInstallReason::Admission);
    config.confirmAdmittedModelBoundary=[](void*)noexcept{return true;};config.exactLoadedWrapperPath="/not/a/loaded/wrapper";
    CHECK(installLoadedNativeSlotPowerObserver(config).reason==ObserverInstallReason::Path);
    config.exactLoadedWrapperPath=canonical(own.dli_fname);config.actualWrapperImageBase=reinterpret_cast<std::uintptr_t>(own.dli_fbase);
    CHECK(installLoadedNativeSlotPowerObserver(config).reason==ObserverInstallReason::DiskImage);
    if(argc==2){Image artifact;CHECK(diskImage(argv[1],artifact)&&artifact.hasUUID&&artifact.plainARM64);}
    CHECK(!nativeSlotPowerObserverStats().installed);
    std::printf("{\"pass\":true,\"checks\":%u,\"allocationFreeCalls\":20000,\"vendorImageLoaded\":false,\"nativeHookInstalled\":false,\"kernelVMOwnDataPageControls\":true}\n",checks);
}
