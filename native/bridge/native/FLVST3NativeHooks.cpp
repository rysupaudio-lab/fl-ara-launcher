#include "FLVST3NativeHooks.h"
#include "RegistrationReadThrough.h"
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <limits.h>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>
#include <type_traits>

// Supplied by the new copied GenericARAHost/owned companion implementation.
// Every hook tests this BEFORE callbacks, registration or native process gating.
namespace rysup::ara_lab { bool ownedPlaybackCompanionCallOnThisThread() noexcept; }

namespace rysup::fl_ara::native_hooks {
using namespace Steinberg;
namespace {
constexpr std::size_t maximumFactoryEntries=64,maximumShadows=2048;
// A fixed aligned virtual reservation lets a hook recover its immutable table
// header directly from its vptr. No RT map/registry/lock. The COMPLETE readable
// native vtable region is COW-remapped, preserving private virtual methods and
// every RTTI/vbase prefix, rather than guessing a standard interface table size.
// Only metadata and selected data-pointer pages become private physical pages.
constexpr mach_vm_size_t shadowReservation=mach_vm_size_t{1}<<26;
constexpr std::uint64_t shadowMagic=0x525953564e485331ULL;
static_assert(std::atomic<void*>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::is_trivial_v<NativeProcessTelemetry>&&std::is_standard_layout_v<NativeProcessTelemetry>);
thread_local unsigned hookDepth=0,nativeProcessDepth=0,originalOnlyDiagnosticStateDepth=0,nativeOriginalStateMethodDepth=0;
struct Runtime;
struct InterfaceSlot;
struct ShadowHeader {std::uint64_t magic=shadowMagic;InterfaceSlot* slot=nullptr;};
enum class Kind {Unknown,PluginBase,Factory,Component,Processor,Controller,ContextRequirements,Handler};
struct InterfaceSlot {
    Runtime* runtime=nullptr;FactoryRecord* factory=nullptr;InstanceRecord* instance=nullptr;
    void* object=nullptr;void** originalTable=nullptr;void** publishedTable=nullptr;
    mach_vm_address_t reservation=0;
    Kind kind=Kind::Unknown;
    // A native host handler can be shared by unrelated controller instances.
    // Its own lifetime and routing never depend on the first controller.
    std::atomic<bool> interfaceNativeAlive{true},handlerSharedAcrossControllers{false};
    std::atomic<InstanceRecord*> handlerFirstController{nullptr};
    std::array<void*,18> original{};
    ImageEvidence tableImage;
};
struct FactoryEntry {Runtime* runtime=nullptr;ModuleRecord* module=nullptr;IPluginFactory* (*original)()=nullptr;};
struct Runtime {
    Config config;
    std::atomic<bool> callbacksEnabled{true};
    std::mutex mutex;
    std::uint64_t nextIdentity=1;
    std::map<std::pair<CFBundleRef,void*>,FactoryEntry*> entries;
    std::map<void*,InterfaceSlot*> interfaces;
    std::vector<std::unique_ptr<ModuleRecord>> modules;
    std::vector<std::unique_ptr<FactoryRecord>> factories;
    std::vector<std::unique_ptr<InstanceRecord>> instances;
    std::vector<std::unique_ptr<InterfaceSlot>> shadows;
    std::vector<void*> nativeCodeLeases;
};
std::atomic<Runtime*> activeRuntime{nullptr};
std::array<std::atomic<FactoryEntry*>,maximumFactoryEntries> factoryEntries{};

ThreadInfo threadInfo(std::uintptr_t actualNativeCaller=0) noexcept {
    return {reinterpret_cast<std::uintptr_t>(pthread_self()),hookDepth,
            pthread_main_np()!=0,nativeProcessDepth!=0,actualNativeCaller};
}
struct HookScope {HookScope()noexcept{++hookDepth;}~HookScope(){--hookDepth;}};
bool bypass()noexcept{return rysup::ara_lab::ownedPlaybackCompanionCallOnThisThread();}
bool callbacks(Runtime* r)noexcept{return r&&r->callbacksEnabled.load(std::memory_order_acquire);}
void fault(Runtime* r,const char* stage,const ModuleRecord* module=nullptr,const InstanceRecord* instance=nullptr)noexcept {
    if(callbacks(r)&&r->config.callbacks.fault)r->config.callbacks.fault(stage,module,instance,threadInfo(),r->config.callbackContext);
}
bool readBytes(const void* source,void* destination,std::size_t size)noexcept {
    if(!source||!destination||!size)return false;
    mach_vm_size_t copied=0;
    return mach_vm_read_overwrite(mach_task_self(),reinterpret_cast<mach_vm_address_t>(source),size,
            reinterpret_cast<mach_vm_address_t>(destination),&copied)==KERN_SUCCESS&&copied==size;
}
std::string canonical(const char* path) {
    char result[PATH_MAX];return path&&realpath(path,result)?result:"";
}
std::string fileSHA(const std::string& path) {
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return {};
    struct stat before{},after{};
    if(fstat(fd,&before)||!S_ISREG(before.st_mode)){close(fd);return {};}
    CC_SHA256_CTX state;CC_SHA256_Init(&state);
    unsigned char buffer[65536];ssize_t count=0;
    while((count=read(fd,buffer,sizeof(buffer)))>0)CC_SHA256_Update(&state,buffer,static_cast<CC_LONG>(count));
    const bool stable=count==0&&!fstat(fd,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino
        &&before.st_size==after.st_size&&before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec
        &&before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec;
    close(fd);if(!stable)return {};
    unsigned char result[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(result,&state);
    static constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(64);
    for(auto byte:result){out+=hex[byte>>4];out+=hex[byte&15];}return out;
}
std::string pathForURL(CFURLRef url) {
    if(!url)return {};UInt8 path[PATH_MAX];
    return CFURLGetFileSystemRepresentation(url,true,path,sizeof(path))?canonical(reinterpret_cast<const char*>(path)):"";
}
ImageEvidence imageEvidence(const void* address) {
    ImageEvidence out;out.actualAddress=reinterpret_cast<std::uintptr_t>(address);Dl_info info{};
    if(address&&dladdr(address,&info)&&info.dli_fname&&info.dli_fbase){
        out.canonicalPath=canonical(info.dli_fname);out.actualImageBase=reinterpret_cast<std::uintptr_t>(info.dli_fbase);
        out.imageOffset=out.actualAddress-out.actualImageBase;out.resolved=!out.canonicalPath.empty();
        if(out.resolved){out.executableSHA256=fileSHA(out.canonicalPath);out.stableRegularFileSHA256=out.executableSHA256.size()==64;}
    }return out;
}
bool knownUID(const ClassID& value,const char* iid)noexcept{return iid&&!memcmp(value.data(),iid,16);}
Kind kindFor(const ClassID& iid)noexcept {
    if(knownUID(iid,INLINE_UID_OF(Vst::IComponent)))return Kind::Component;
    if(knownUID(iid,INLINE_UID_OF(Vst::IAudioProcessor)))return Kind::Processor;
    if(knownUID(iid,INLINE_UID_OF(Vst::IEditController)))return Kind::Controller;
    if(knownUID(iid,INLINE_UID_OF(Vst::IProcessContextRequirements)))return Kind::ContextRequirements;
    if(knownUID(iid,INLINE_UID_OF(IPluginBase)))return Kind::PluginBase;
    return Kind::Unknown;
}
bool factoryUID(const ClassID& iid)noexcept {
    return knownUID(iid,INLINE_UID_OF(IPluginFactory))||knownUID(iid,INLINE_UID_OF(IPluginFactory2))
        ||knownUID(iid,INLINE_UID_OF(IPluginFactory3));
}
InterfaceSlot* slotFor(void* self)noexcept {
    // Only our published vptr can enter these functions. Entire native tables
    // remain intact; cloned/otherwise unregistered self forwards via originals.
    auto** table=__atomic_load_n(reinterpret_cast<void***>(self),__ATOMIC_ACQUIRE);
    auto* header=reinterpret_cast<ShadowHeader*>(reinterpret_cast<std::uintptr_t>(table)&~(shadowReservation-1));
    return header->magic==shadowMagic?header->slot:nullptr;
}
bool actualObject(InterfaceSlot* slot,void* self)noexcept {
    return slot&&slot->object==self&&slot->interfaceNativeAlive.load(std::memory_order_acquire)
        &&(!slot->instance||slot->instance->nativeAlive.load(std::memory_order_acquire))
        &&(!slot->factory||slot->factory->nativeAlive.load(std::memory_order_acquire));
}
template<class Function>Function original(InterfaceSlot* slot,unsigned index)noexcept {
    return reinterpret_cast<Function>(slot->original[index]);
}

// No native instruction is changed. This CAS touches object/vtable DATA only.
struct PointerChange {bool changed=false,protectionRestored=false;};
PointerChange replaceDataPointer(void* location,void* expected,void* replacement)noexcept {
    PointerChange result;const auto address=reinterpret_cast<mach_vm_address_t>(location);
    mach_vm_address_t region=address;mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info{};mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t memory=MACH_PORT_NULL;
    if(mach_vm_region(mach_task_self(),&region,&size,VM_REGION_BASIC_INFO_64,reinterpret_cast<vm_region_info_t>(&info),&count,&memory)!=KERN_SUCCESS)return result;
    if(memory!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),memory);
    if(address<region||address+sizeof(void*)>region+size||!(info.protection&VM_PROT_READ)||(info.protection&VM_PROT_EXECUTE))return result;
    const auto page=address&~(static_cast<mach_vm_address_t>(vm_page_size)-1);const bool wasWritable=info.protection&VM_PROT_WRITE;
    if(!wasWritable&&mach_vm_protect(mach_task_self(),page,vm_page_size,false,info.protection|VM_PROT_WRITE|VM_PROT_COPY)!=KERN_SUCCESS)return result;
    result.changed=__atomic_compare_exchange_n(reinterpret_cast<void**>(location),&expected,replacement,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    result.protectionRestored=wasWritable||mach_vm_protect(mach_task_self(),page,vm_page_size,false,info.protection)==KERN_SUCCESS;
    if(!result.protectionRestored){
        // Fail closed: withdraw this data publication before retrying the exact
        // original protection. No generated code or instruction-page fallback.
        if(result.changed){
            void* current=replacement;
            if(__atomic_compare_exchange_n(reinterpret_cast<void**>(location),&current,expected,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
                result.changed=false;
        }
        result.protectionRestored=mach_vm_protect(mach_task_self(),page,vm_page_size,false,info.protection)==KERN_SUCCESS;
    }
    return result;
}

using Query=tresult(PLUGIN_API *)(void*,const TUID,void**);
using Release=uint32(PLUGIN_API *)(void*);
using Initialize=tresult(PLUGIN_API *)(void*,FUnknown*);
using Terminate=tresult(PLUGIN_API *)(void*);
using StateFunction=OriginalStateCall::Function;
using Toggle=tresult(PLUGIN_API *)(void*,TBool);
using Process=tresult(PLUGIN_API *)(void*,Vst::ProcessData&);
using Setup=tresult(PLUGIN_API *)(void*,Vst::ProcessSetup&);
using Requirements=uint32(PLUGIN_API *)(void*);
tresult invokeOriginalState(StateFunction function,void* self,IBStream* stream) {
    struct NativeStateScope {
        NativeStateScope()noexcept{++nativeOriginalStateMethodDepth;}
        ~NativeStateScope(){--nativeOriginalStateMethodDepth;}
    } actualStateMethod;
    return function(self,stream);
}

tresult PLUGIN_API queryInterface(void*,const TUID,void**);
uint32 PLUGIN_API releaseInterface(void*);
tresult PLUGIN_API initializeInterface(void*,FUnknown*);
tresult PLUGIN_API terminateInterface(void*);
tresult PLUGIN_API componentSetActive(void*,TBool);
tresult PLUGIN_API componentSetState(void*,IBStream*);
tresult PLUGIN_API componentGetState(void*,IBStream*);
tresult PLUGIN_API controllerComponentState(void*,IBStream*);
tresult PLUGIN_API processorSetup(void*,Vst::ProcessSetup&);
tresult PLUGIN_API processorSetProcessing(void*,TBool);
tresult PLUGIN_API processorProcess(void*,Vst::ProcessData&);
uint32 PLUGIN_API processContextRequirements(void*);
IPlugView* PLUGIN_API controllerCreateView(void*,FIDString);
tresult PLUGIN_API controllerSetParamNormalized(void*,Vst::ParamID,Vst::ParamValue);
tresult PLUGIN_API controllerSetComponentHandler(void*,Vst::IComponentHandler*);
tresult PLUGIN_API handlerPerformEdit(void*,Vst::ParamID,Vst::ParamValue);
tresult PLUGIN_API factoryInfo(void*,PFactoryInfo*);
int32 PLUGIN_API factoryClassCount(void*);
tresult PLUGIN_API factoryClassInfo(void*,int32,PClassInfo*);
tresult PLUGIN_API factoryCreateInstance(void*,FIDString,FIDString,void**);

std::vector<std::pair<unsigned,void*>> replacements(Kind kind) {
    std::vector<std::pair<unsigned,void*>> out{{0,reinterpret_cast<void*>(&queryInterface)},{2,reinterpret_cast<void*>(&releaseInterface)}};
    if(kind==Kind::Factory){out.insert(out.end(),{{3,reinterpret_cast<void*>(&factoryInfo)},{4,reinterpret_cast<void*>(&factoryClassCount)},
        {5,reinterpret_cast<void*>(&factoryClassInfo)},{6,reinterpret_cast<void*>(&factoryCreateInstance)}});}
    else if(kind==Kind::Component||kind==Kind::Controller||kind==Kind::PluginBase){
        out.insert(out.end(),{{3,reinterpret_cast<void*>(&initializeInterface)},{4,reinterpret_cast<void*>(&terminateInterface)}});
        if(kind==Kind::Component)out.insert(out.end(),{{11,reinterpret_cast<void*>(&componentSetActive)},
            {12,reinterpret_cast<void*>(&componentSetState)},{13,reinterpret_cast<void*>(&componentGetState)}});
        if(kind==Kind::Controller)out.insert(out.end(),{{5,reinterpret_cast<void*>(&controllerComponentState)},
            {15,reinterpret_cast<void*>(&controllerSetParamNormalized)},{16,reinterpret_cast<void*>(&controllerSetComponentHandler)},
            {17,reinterpret_cast<void*>(&controllerCreateView)}});
    }else if(kind==Kind::Processor)out.insert(out.end(),{{7,reinterpret_cast<void*>(&processorSetup)},
        {8,reinterpret_cast<void*>(&processorSetProcessing)},{9,reinterpret_cast<void*>(&processorProcess)}});
    else if(kind==Kind::ContextRequirements)out.push_back({3,reinterpret_cast<void*>(&processContextRequirements)});
    else if(kind==Kind::Handler)out.push_back({4,reinterpret_cast<void*>(&handlerPerformEdit)});
    return out;
}
bool allocateShadow(InterfaceSlot& slot,void** nativeTable) {
    mach_vm_address_t region=reinterpret_cast<mach_vm_address_t>(nativeTable);mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info{};mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t memory=MACH_PORT_NULL;
    if(mach_vm_region(mach_task_self(),&region,&size,VM_REGION_BASIC_INFO_64,reinterpret_cast<vm_region_info_t>(&info),&count,&memory)!=KERN_SUCCESS)return false;
    if(memory!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),memory);
    const auto address=reinterpret_cast<mach_vm_address_t>(nativeTable);
    if(!(info.protection&VM_PROT_READ)||(info.protection&VM_PROT_EXECUTE)||address<region+16||size+vm_page_size>shadowReservation)return false;
    mach_vm_address_t reserve=0;
    if(mach_vm_allocate(mach_task_self(),&reserve,shadowReservation*2,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS)return false;
    const auto aligned=(reserve+shadowReservation-1)&~(shadowReservation-1);
    if(aligned>reserve)mach_vm_deallocate(mach_task_self(),reserve,aligned-reserve);
    if(aligned+shadowReservation<reserve+shadowReservation*2)mach_vm_deallocate(mach_task_self(),aligned+shadowReservation,reserve+shadowReservation*2-aligned-shadowReservation);
    slot.reservation=aligned;
    new(reinterpret_cast<void*>(aligned))ShadowHeader{shadowMagic,&slot};
    auto target=aligned+vm_page_size;vm_prot_t current=0,maximum=0;
    if(mach_vm_remap(mach_task_self(),&target,size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,mach_task_self(),region,true,&current,&maximum,VM_INHERIT_NONE)!=KERN_SUCCESS)return false;
    slot.originalTable=nativeTable;slot.publishedTable=reinterpret_cast<void**>(target+address-region);
    for(auto [index,replacement]:replacements(slot.kind)){
        if(address+(index+1)*sizeof(void*)>region+size)return false;
        void* value=nullptr;
        if(!readBytes(nativeTable+index,&value,sizeof(value))||!value)return false;
        Dl_info method{};if(!dladdr(value,&method)||!method.dli_fbase||!method.dli_fname)return false;
        slot.original[index]=value;
        const auto changed=replaceDataPointer(slot.publishedTable+index,value,replacement);
        if(!changed.changed||!changed.protectionRestored)return false;
    }
    return true;
}

InterfaceSlot* publishInterface(Runtime*,void*,Kind,FactoryRecord*,InstanceRecord*,InstanceRecord* diagnosticController=nullptr);
void observeInterface(Runtime* runtime,void* object,Kind kind,FactoryRecord* factory,InstanceRecord* instance) {
    auto* slot=publishInterface(runtime,object,kind,factory,instance);
    if(!slot){fault(runtime,"native_interface_data_hook_not_installed",factory?factory->module:nullptr,instance);return;}
    if(!instance)return;
    if(kind==Kind::Component)instance->actualComponent.store(static_cast<Vst::IComponent*>(object),std::memory_order_release);
    else if(kind==Kind::Processor)instance->actualProcessor.store(static_cast<Vst::IAudioProcessor*>(object),std::memory_order_release);
    else if(kind==Kind::Controller)instance->actualController.store(static_cast<Vst::IEditController*>(object),std::memory_order_release);
    if(callbacks(runtime)&&runtime->config.callbacks.interfaceObserved){
        ClassID iid{};
        const char* value=kind==Kind::Component?INLINE_UID_OF(Vst::IComponent):kind==Kind::Processor?INLINE_UID_OF(Vst::IAudioProcessor):
            kind==Kind::Controller?INLINE_UID_OF(Vst::IEditController):kind==Kind::ContextRequirements?INLINE_UID_OF(Vst::IProcessContextRequirements):INLINE_UID_OF(FUnknown);
        memcpy(iid.data(),value,16);
        runtime->config.callbacks.interfaceObserved(*instance,iid,object,slot->tableImage,threadInfo(),runtime->config.callbackContext);
    }
}
InterfaceSlot* publishInterface(Runtime* runtime,void* object,Kind kind,FactoryRecord* factory,InstanceRecord* instance,InstanceRecord* diagnosticController) {
    if(!runtime||!object||nativeProcessDepth||bypass())return nullptr;
    std::unique_lock<std::mutex> lock(runtime->mutex);
    void** table=nullptr;if(!readBytes(object,&table,sizeof(table)))return nullptr;
    auto found=runtime->interfaces.find(object);
    if(found!=runtime->interfaces.end()&&found->second->publishedTable==table){
        auto* existing=found->second;
        if(kind==Kind::Handler&&existing->kind==Kind::Handler&&diagnosticController&&
                existing->handlerFirstController.load(std::memory_order_acquire)!=diagnosticController)
            existing->handlerSharedAcrossControllers.store(true,std::memory_order_release);
        if(existing->kind==kind||kind==Kind::Unknown||existing->kind!=Kind::Unknown&&existing->kind!=Kind::PluginBase)return existing;
        // Promote an originally requested FUnknown/IPluginBase on a real native
        // QI without another createInstance, initialize or reference-count call.
        table=existing->originalTable;
    }
    if(runtime->shadows.size()>=maximumShadows)return nullptr;
    auto slot=std::make_unique<InterfaceSlot>();slot->runtime=runtime;slot->object=object;slot->kind=kind;
    slot->factory=factory;slot->instance=instance;slot->tableImage=imageEvidence(table);
    slot->handlerFirstController.store(diagnosticController,std::memory_order_relaxed);
    if(!allocateShadow(*slot,table)){
        if(slot->reservation)mach_vm_deallocate(mach_task_self(),slot->reservation,shadowReservation);
        return nullptr;
    }
    void* expected=found!=runtime->interfaces.end()&&found->second->publishedTable==__atomic_load_n(reinterpret_cast<void***>(object),__ATOMIC_ACQUIRE)
        ?reinterpret_cast<void*>(found->second->publishedTable):reinterpret_cast<void*>(table);
    auto* result=slot.get();
    // Own immutable header/originals are fully published before the data CAS.
    runtime->shadows.push_back(std::move(slot));
    const auto changed=replaceDataPointer(object,expected,result->publishedTable);
    if(!changed.changed||__atomic_load_n(reinterpret_cast<void***>(object),__ATOMIC_ACQUIRE)!=result->publishedTable)return nullptr;
    runtime->interfaces[object]=result;
    lock.unlock();
    if(!changed.protectionRestored)
        fault(runtime,"object_data_protection_restore_failed",factory?factory->module:nullptr,instance);
    // Keep every exposed table valid, even after a protection-restoration fault.
    return result;
}

// State objects and module/data-shadow leases intentionally outlive all native
// records, including final-release callbacks; no borrowed self is kept alive.
FactoryRecord* observeFactory(FactoryEntry& entry,IPluginFactory* actual) {
    if(!actual)return nullptr;auto* runtime=entry.runtime;
    FactoryRecord* record=nullptr;
    {
        std::unique_lock<std::mutex> lock(runtime->mutex);
        auto found=runtime->interfaces.find(actual);
        if(found!=runtime->interfaces.end()&&found->second->factory&&found->second->factory->nativeAlive.load()
            &&found->second->factory->actualFactory==actual&&found->second->publishedTable==
                __atomic_load_n(reinterpret_cast<void***>(actual),__ATOMIC_ACQUIRE)){
            auto* existing=found->second->factory;
            const bool ambiguous=existing->module!=entry.module;
            if(ambiguous)existing->moduleAssociationAmbiguous.store(true,std::memory_order_release);
            lock.unlock();
            if(ambiguous)fault(runtime,"same_native_factory_returned_by_distinct_bundle_entries",entry.module);
            return existing;
        }
        auto value=std::make_unique<FactoryRecord>();value->identity=runtime->nextIdentity++;
        value->module=entry.module;value->actualFactory=actual;record=value.get();runtime->factories.push_back(std::move(value));
    }
    if(!publishInterface(runtime,actual,Kind::Factory,record,nullptr)){fault(runtime,"factory_data_hook_not_installed",entry.module);return nullptr;}
    if(callbacks(runtime)&&runtime->config.callbacks.factoryObserved)runtime->config.callbacks.factoryObserved(*record,threadInfo(),runtime->config.callbackContext);
    return record;
}

template<std::size_t Index>IPluginFactory* factoryThunk() {
    auto* entry=factoryEntries[Index].load(std::memory_order_acquire);
    if(!entry||!entry->original)return nullptr;
    // The actual vendor factory entry is invoked exactly once, including bypass.
    IPluginFactory* result=entry->original();
    if(bypass()||nativeProcessDepth)return result;
    HookScope scope;
    try{observeFactory(*entry,result);}catch(...){fault(entry->runtime,"factory_observation_failed",entry->module);}
    return result;
}
template<std::size_t... Index>constexpr auto thunkArray(std::index_sequence<Index...>) {
    return std::array<IPluginFactory*(*)(),sizeof...(Index)>{{&factoryThunk<Index>...}};
}
constexpr auto factoryThunks=thunkArray(std::make_index_sequence<maximumFactoryEntries>{});

FactoryEntry* registerFactoryEntry(Runtime* runtime,CFBundleRef bundle,void* originalEntry) {
    std::unique_lock<std::mutex> lock(runtime->mutex);
    const auto key=std::make_pair(bundle,originalEntry);
    if(auto found=runtime->entries.find(key);found!=runtime->entries.end())return found->second;
    std::size_t index=0;
    while(index<factoryEntries.size()&&factoryEntries[index].load(std::memory_order_acquire))++index;
    if(index==factoryEntries.size())return nullptr;
    auto module=std::make_unique<ModuleRecord>();module->identity=runtime->nextIdentity++;
    auto url=CFBundleCopyBundleURL(bundle);module->canonicalBundlePath=pathForURL(url);if(url)CFRelease(url);
    auto executable=CFBundleCopyExecutableURL(bundle);module->canonicalExecutablePath=pathForURL(executable);if(executable)CFRelease(executable);
    const auto& name=module->canonicalBundlePath;
    if(name.size()<5||strcasecmp(name.c_str()+name.size()-5,".vst3")||module->canonicalExecutablePath.empty())return nullptr;
    module->executableSHA256=fileSHA(module->canonicalExecutablePath);
    module->stableExecutableSHA256=module->executableSHA256.size()==64;
    module->actualFactoryEntry=originalEntry;module->actualFactoryEntryImage=imageEvidence(originalEntry);
    if(!module->stableExecutableSHA256||!module->actualFactoryEntryImage.resolved)return nullptr;
    CFRetain(bundle);module->actualBundle=bundle;
    // Source may reside in a genuine vendor framework. Keep separate identities
    // and NOLOAD leases; never equate its address image to the bundle executable.
    void* executableLease=dlopen(module->canonicalExecutablePath.c_str(),RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);
    void* factoryLease=dlopen(module->actualFactoryEntryImage.canonicalPath.c_str(),RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);
    if(executableLease)runtime->nativeCodeLeases.push_back(executableLease);
    if(factoryLease)runtime->nativeCodeLeases.push_back(factoryLease);
    module->nativeCodeLeaseRetained=executableLease&&factoryLease;
    auto* record=module.get();runtime->modules.push_back(std::move(module));
    auto* entry=new FactoryEntry{runtime,record,reinterpret_cast<IPluginFactory*(*)()>(originalEntry)};
    runtime->entries[key]=entry;
    factoryEntries[index].store(entry,std::memory_order_release);
    lock.unlock();
    if(callbacks(runtime)&&runtime->config.callbacks.moduleObserved)runtime->config.callbacks.moduleObserved(*record,threadInfo(),runtime->config.callbackContext);
    return entry;
}
void* observeCFBundleGetFunctionPointerForName(CFBundleRef bundle,CFStringRef symbol) {
    // dyld excludes this replacement image's direct replacee reference.
    void* actual=CFBundleGetFunctionPointerForName(bundle,symbol);
    // Same optional, independently sealed registration read-through used by the
    // native app's dlsym observer. It receives the REAL resolver result before
    // any public plug-in/runtime/TLS filter. The adapter is inert unless its
    // separate exact copied-libwinbridge/readonly-subtree admission is enabled.
    // Real registration symbol names are short ASCII. Unknown/unconvertible or
    // overlong names preserve the actual resolver result without another load.
    char utf8Name[256];
    if(symbol&&CFStringGetCString(symbol,utf8Name,sizeof(utf8Name),kCFStringEncodingUTF8))
        actual=::rysup::fl_ara::observeRegistrationSymbol(utf8Name,actual);
    auto* runtime=activeRuntime.load(std::memory_order_acquire);
    if(!runtime||bypass()||nativeProcessDepth||!bundle||!symbol||!actual||!CFEqual(symbol,CFSTR("GetPluginFactory")))return actual;
    HookScope scope;
    try{
        auto* entry=registerFactoryEntry(runtime,bundle,actual);
        if(!entry){fault(runtime,"factory_entry_capacity_or_source_unavailable");return actual;}
        for(std::size_t i=0;i<factoryEntries.size();++i)
            if(factoryEntries[i].load(std::memory_order_acquire)==entry)return reinterpret_cast<void*>(factoryThunks[i]);
    }catch(...){fault(runtime,"factory_entry_registration_failed");}
    return actual;
}

InstanceRecord* createdInstance(InterfaceSlot& factorySlot,void* object,const ClassID& cid,const ClassID& iid) {
    auto* runtime=factorySlot.runtime;auto kind=kindFor(iid);
    const bool unknown=knownUID(iid,INLINE_UID_OF(FUnknown));
    if(kind==Kind::Unknown&&!unknown)return nullptr; // ARA/custom providers are not native audio instances.
    InstanceRecord* record=nullptr;
    {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        auto found=runtime->interfaces.find(object);
        if(found!=runtime->interfaces.end()&&found->second->instance&&found->second->instance->nativeAlive.load()
            &&found->second->instance->factory==factorySlot.factory&&found->second->publishedTable==
                __atomic_load_n(reinterpret_cast<void***>(object),__ATOMIC_ACQUIRE))return found->second->instance;
        auto value=std::make_unique<InstanceRecord>();value->identity=runtime->nextIdentity++;
        value->factory=factorySlot.factory;value->actualRequestedClassID=cid;value->actualRequestedInterfaceID=iid;
        value->createdKind=kind==Kind::Component?CreatedKind::Component:kind==Kind::Controller?CreatedKind::Controller:CreatedKind::Unknown;
        record=value.get();runtime->instances.push_back(std::move(value));
    }
    observeInterface(runtime,object,kind,factorySlot.factory,record);
    if(callbacks(runtime)&&runtime->config.callbacks.instanceCreated)runtime->config.callbacks.instanceCreated(*record,threadInfo(),runtime->config.callbackContext);
    return record;
}
tresult PLUGIN_API queryInterface(void* self,const TUID requested,void** output) {
    auto* slot=slotFor(self);auto function=original<Query>(slot,0);
    if(bypass()||nativeProcessDepth||!actualObject(slot,self))return function(self,requested,output);
    HookScope scope;ClassID iid{};const bool copied=readBytes(requested,iid.data(),iid.size());
    const auto result=function(self,requested,output);
    if(result!=kResultOk||!copied)return result;
    void* value=nullptr;if(!readBytes(output,&value,sizeof(value))||!value)return result;
    try{
        if(slot->instance){
            const auto kind=kindFor(iid);
            if(kind!=Kind::Unknown||knownUID(iid,INLINE_UID_OF(FUnknown)))
                observeInterface(slot->runtime,value,kind,slot->factory,slot->instance);
        }else if(slot->factory&&factoryUID(iid))publishInterface(slot->runtime,value,Kind::Factory,slot->factory,nullptr);
        else if(slot->kind==Kind::Handler&&knownUID(iid,INLINE_UID_OF(Vst::IComponentHandler)))
            publishInterface(slot->runtime,value,Kind::Handler,nullptr,nullptr,slot->handlerFirstController.load(std::memory_order_acquire));
    }catch(...){fault(slot->runtime,"native_query_interface_observation_failed",slot->factory?slot->factory->module:nullptr,slot->instance);}
    return result;
}
uint32 PLUGIN_API releaseInterface(void* self) {
    auto* slot=slotFor(self);auto function=original<Release>(slot,2);
    const bool owned=actualObject(slot,self)&&!bypass()&&!nativeProcessDepth;
    const auto result=function(self); // self may have been deleted; never read it again.
    if(!owned||result!=0)return result;
    HookScope scope;
    slot->interfaceNativeAlive.store(false,std::memory_order_release);
    if(slot->instance){
        slot->instance->nativeAlive.store(false,std::memory_order_release);
        auto* runtime=slot->runtime;
        if(callbacks(runtime)&&runtime->config.callbacks.finalReleaseObserved)
            runtime->config.callbacks.finalReleaseObserved(*slot->instance,threadInfo(),runtime->config.callbackContext);
    }else if(slot->factory)slot->factory->nativeAlive.store(false,std::memory_order_release);
    return result;
}
tresult PLUGIN_API factoryInfo(void* self,PFactoryInfo* info) {
    auto* slot=slotFor(self);auto function=original<tresult(PLUGIN_API *)(void*,PFactoryInfo*)>(slot,3);
    const auto result=function(self,info);
    if(bypass()||nativeProcessDepth||!actualObject(slot,self)||result!=kResultOk)return result;
    HookScope scope;PFactoryInfo copy{};auto* runtime=slot->runtime;
    if(readBytes(info,&copy,sizeof(copy))&&callbacks(runtime)&&runtime->config.callbacks.factoryInfoObserved)
        runtime->config.callbacks.factoryInfoObserved(*slot->factory,copy,threadInfo(),runtime->config.callbackContext);
    return result;
}
int32 PLUGIN_API factoryClassCount(void* self) {
    auto* slot=slotFor(self);auto function=original<int32(PLUGIN_API *)(void*)>(slot,4);
    return function(self); // no additional factory queries, inferred classes or registrations
}
tresult PLUGIN_API factoryClassInfo(void* self,int32 index,PClassInfo* info) {
    auto* slot=slotFor(self);auto function=original<tresult(PLUGIN_API *)(void*,int32,PClassInfo*)>(slot,5);
    const auto result=function(self,index,info);
    if(bypass()||nativeProcessDepth||!actualObject(slot,self)||result!=kResultOk)return result;
    HookScope scope;PClassInfo copy{};auto* runtime=slot->runtime;
    if(readBytes(info,&copy,sizeof(copy))&&callbacks(runtime)&&runtime->config.callbacks.classInfoObserved)
        runtime->config.callbacks.classInfoObserved(*slot->factory,index,copy,threadInfo(),runtime->config.callbackContext);
    return result;
}
tresult PLUGIN_API factoryCreateInstance(void* self,FIDString cid,FIDString iid,void** output) {
    auto* slot=slotFor(self);auto function=original<tresult(PLUGIN_API *)(void*,FIDString,FIDString,void**)>(slot,6);
    if(bypass()||nativeProcessDepth||!actualObject(slot,self))return function(self,cid,iid,output);
    HookScope scope;ClassID classID{},interfaceID{};
    const bool copied=readBytes(cid,classID.data(),16)&&readBytes(iid,interfaceID.data(),16);
    const auto result=function(self,cid,iid,output);
    void* value=nullptr;
    if(result==kResultOk&&copied&&readBytes(output,&value,sizeof(value))&&value){
        try{createdInstance(*slot,value,classID,interfaceID);}catch(...){fault(slot->runtime,"native_create_instance_observation_failed",slot->factory->module);}
    }return result;
}
__attribute__((noinline)) tresult PLUGIN_API initializeInterface(void* self,FUnknown* context) {
    const auto actualNativeCaller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto* slot=slotFor(self);auto function=original<Initialize>(slot,3);
    if(bypass()||!actualObject(slot,self))return function(self,context);
    HookScope scope;auto& instance=*slot->instance;
    instance.initializeCalls.fetch_add(1,std::memory_order_relaxed);
    instance.actualInitializeContext.store(context,std::memory_order_release);
    const auto result=function(self,context);
    if(result==kResultOk)instance.nativeInitialized.store(true,std::memory_order_release);
    auto* runtime=slot->runtime;
    if(callbacks(runtime)&&runtime->config.callbacks.initialized)
        runtime->config.callbacks.initialized(instance,self,context,result,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    return result;
}
tresult PLUGIN_API terminateInterface(void* self) {
    const auto actualNativeCaller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto* slot=slotFor(self);auto function=original<Terminate>(slot,4);
    if(bypass()||!actualObject(slot,self))return function(self);
    HookScope scope;auto& instance=*slot->instance;auto* runtime=slot->runtime;
    instance.terminateCalls.fetch_add(1,std::memory_order_relaxed);
    if(callbacks(runtime)&&runtime->config.callbacks.beforeTerminate)
        runtime->config.callbacks.beforeTerminate(instance,self,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    const auto result=function(self);
    if(result==kResultOk){instance.nativeTerminated.store(true,std::memory_order_release);instance.nativeInitialized.store(false,std::memory_order_release);}
    if(callbacks(runtime)&&runtime->config.callbacks.terminated)
        runtime->config.callbacks.terminated(instance,self,result,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    return result;
}
tresult PLUGIN_API componentSetActive(void* self,TBool value) {
    const auto actualNativeCaller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto* slot=slotFor(self);auto function=original<Toggle>(slot,11);
    if(bypass()||!actualObject(slot,self))return function(self,value);
    HookScope scope;auto& instance=*slot->instance;
    instance.activationCalls.fetch_add(1,std::memory_order_relaxed);
    const auto result=function(self,value);auto* runtime=slot->runtime;
    if(callbacks(runtime)&&runtime->config.callbacks.setActiveObserved)
        runtime->config.callbacks.setActiveObserved(instance,value,result,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    return result;
}
tresult stateCall(void* self,IBStream* stream,unsigned index,StateOperation operation,std::uintptr_t actualNativeCaller) {
    auto* slot=slotFor(self);auto function=original<StateFunction>(slot,index);
    if(bypass()||originalOnlyDiagnosticStateDepth||!actualObject(slot,self))return invokeOriginalState(function,self,stream);
    HookScope scope;auto& instance=*slot->instance;auto* runtime=slot->runtime;
    (operation==StateOperation::ComponentGetState?instance.getStateCalls:instance.setStateCalls).fetch_add(1,std::memory_order_relaxed);
    if(!callbacks(runtime)||!instance.stateInterceptionEnabled.load(std::memory_order_acquire)||!runtime->config.callbacks.state)
        return invokeOriginalState(function,self,stream);
    OriginalStateCall originalCall(self,function);
    const auto result=runtime->config.callbacks.state(instance,operation,stream,originalCall,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    if(originalCall.duplicateCallAttempted()||!originalCall.wasCalled()&&result==kResultOk)return kInternalError;
    return result;
}
__attribute__((noinline)) tresult PLUGIN_API componentSetState(void* self,IBStream* stream){return stateCall(self,stream,12,StateOperation::ComponentSetState,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));}
__attribute__((noinline)) tresult PLUGIN_API componentGetState(void* self,IBStream* stream){return stateCall(self,stream,13,StateOperation::ComponentGetState,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));}
__attribute__((noinline)) tresult PLUGIN_API controllerComponentState(void* self,IBStream* stream){return stateCall(self,stream,5,StateOperation::ControllerSetComponentState,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));}
tresult PLUGIN_API controllerSetParamNormalized(void* self,Vst::ParamID id,Vst::ParamValue value) {
    auto* slot=slotFor(self);
    auto function=original<tresult(PLUGIN_API *)(void*,Vst::ParamID,Vst::ParamValue)>(slot,15);
    if(bypass()||!actualObject(slot,self))return function(self,id,value);
    HookScope scope;const auto result=function(self,id,value);auto* runtime=slot->runtime;
    if(result==kResultOk&&callbacks(runtime)&&runtime->config.callbacks.paramNormalizedObserved)
        runtime->config.callbacks.paramNormalizedObserved(*slot->instance,static_cast<Vst::IEditController*>(self),
            id,value,threadInfo(),runtime->config.callbackContext);
    return result;
}
tresult PLUGIN_API controllerSetComponentHandler(void* self,Vst::IComponentHandler* handler) {
    auto* slot=slotFor(self);
    auto function=original<tresult(PLUGIN_API *)(void*,Vst::IComponentHandler*)>(slot,16);
    if(bypass()||!actualObject(slot,self))return function(self,handler);
    HookScope scope;
    auto& instance=*slot->instance;auto* runtime=slot->runtime;
    struct PendingHandlerScope {
        InstanceRecord& instance;Vst::IComponentHandler* previous;
        bool restored=false;
        PendingHandlerScope(InstanceRecord& value,Vst::IComponentHandler* actual)noexcept
            :instance(value),previous(value.actualPendingNativeComponentHandler.exchange(actual,std::memory_order_acq_rel)){}
        void restore()noexcept{instance.actualPendingNativeComponentHandler.store(previous,std::memory_order_release);restored=true;}
        ~PendingHandlerScope(){if(!restored)restore();}
    } pending(instance,handler);
    if(callbacks(runtime)&&runtime->config.callbacks.controllerHandlerObserved)
        runtime->config.callbacks.controllerHandlerObserved(instance,ControllerHandlerPhase::BeforeOriginal,
            static_cast<Vst::IEditController*>(self),handler,kInternalError,threadInfo(),runtime->config.callbackContext);
    // Public SDK setter is a UI-thread call. Registration touches metadata only
    // there; unexpected processing/background calls still forward exactly once.
    // Publish before that call to catch a synchronous performEdit from it. This
    // pointer was actually passed, even if the setter later refuses it.
    if(handler&&!nativeProcessDepth&&pthread_main_np()){
        try{
            auto* handlerSlot=publishInterface(slot->runtime,handler,Kind::Handler,nullptr,nullptr,slot->instance);
            if(!handlerSlot||handlerSlot->kind!=Kind::Handler)
                fault(slot->runtime,"actual_native_handler_data_hook_not_installed",slot->factory?slot->factory->module:nullptr,slot->instance);
        }catch(...){fault(slot->runtime,"actual_native_handler_registration_failed",slot->factory?slot->factory->module:nullptr,slot->instance);}
    }
    const auto result=function(self,handler);
    if(result==kResultOk)instance.actualNativeComponentHandler.store(handler,std::memory_order_release);
    pending.restore();
    if(callbacks(runtime)&&runtime->config.callbacks.controllerHandlerObserved)
        runtime->config.callbacks.controllerHandlerObserved(instance,ControllerHandlerPhase::AfterOriginal,
            static_cast<Vst::IEditController*>(self),handler,result,threadInfo(),runtime->config.callbackContext);
    return result;
}
tresult PLUGIN_API handlerPerformEdit(void* self,Vst::ParamID id,Vst::ParamValue value) {
    auto* slot=slotFor(self);
    auto function=original<tresult(PLUGIN_API *)(void*,Vst::ParamID,Vst::ParamValue)>(slot,4);
    if(bypass()||!actualObject(slot,self))return function(self,id,value);
    HookScope scope;const auto result=function(self,id,value);auto* runtime=slot->runtime;
    // Direct immutable shadow metadata + atomics only. A shared handler gives
    // no truthful edited-insert identity: first controller is diagnostics only.
    if(result==kResultOk&&callbacks(runtime)&&runtime->config.callbacks.paramEditObserved)
        runtime->config.callbacks.paramEditObserved(slot->handlerFirstController.load(std::memory_order_acquire),
            static_cast<Vst::IComponentHandler*>(self),id,value,threadInfo(),runtime->config.callbackContext);
    return result;
}
tresult PLUGIN_API processorSetup(void* self,Vst::ProcessSetup& setup) {
    const auto actualNativeCaller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto* slot=slotFor(self);auto function=original<Setup>(slot,7);
    if(bypass()||!actualObject(slot,self))return function(self,setup);
    HookScope scope;const auto result=function(self,setup);auto* runtime=slot->runtime;
    if(callbacks(runtime)&&runtime->config.callbacks.setupProcessingObserved)
        runtime->config.callbacks.setupProcessingObserved(*slot->instance,setup,result,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    return result;
}
tresult PLUGIN_API processorSetProcessing(void* self,TBool value) {
    const auto actualNativeCaller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto* slot=slotFor(self);auto function=original<Toggle>(slot,8);
    if(bypass()||!actualObject(slot,self))return function(self,value);
    HookScope scope;slot->instance->processingCalls.fetch_add(1,std::memory_order_relaxed);
    const auto result=function(self,value);auto* runtime=slot->runtime;
    if(callbacks(runtime)&&runtime->config.callbacks.setProcessingObserved)
        runtime->config.callbacks.setProcessingObserved(*slot->instance,value,result,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    return result;
}
tresult zeroActualOutputs(Vst::ProcessData& data)noexcept {
    if(data.numSamples<0||data.numOutputs<0||data.numOutputs>1024)return kInvalidArgument;
    if(data.numOutputs&&!data.outputs)return kInvalidArgument;
    if(data.symbolicSampleSize!=Vst::kSample32&&data.symbolicSampleSize!=Vst::kSample64)return kInvalidArgument;
    const auto width=data.symbolicSampleSize==Vst::kSample32?sizeof(Vst::Sample32):sizeof(Vst::Sample64);
    for(int32 bus=0;bus<data.numOutputs;++bus){auto& output=data.outputs[bus];
        if(output.numChannels<0||output.numChannels>4096)return kInvalidArgument;
        if(data.numSamples&&output.numChannels&&
            (data.symbolicSampleSize==Vst::kSample32?!output.channelBuffers32:!output.channelBuffers64))return kInvalidArgument;
        for(int32 channel=0;data.numSamples&&channel<output.numChannels;++channel){
            void* buffer=data.symbolicSampleSize==Vst::kSample32?static_cast<void*>(output.channelBuffers32[channel]):static_cast<void*>(output.channelBuffers64[channel]);
            if(data.numSamples&&!buffer)return kInvalidArgument;
            if(data.numSamples)memset(buffer,0,static_cast<std::size_t>(data.numSamples)*width);
        }
        output.silenceFlags=output.numChannels>=64?~uint64{0}:output.numChannels?((uint64{1}<<output.numChannels)-1):0;
    }return kResultOk;
}
tresult PLUGIN_API processorProcess(void* self,Vst::ProcessData& data) {
    auto* slot=slotFor(self);auto function=original<Process>(slot,9);
    if(bypass()||!actualObject(slot,self))return function(self,data);
    auto& instance=*slot->instance;
    auto flight=instance.nativeProcessAdmission.tryEnter();
    if(!flight){
        instance.blockedProcessCallbacks.fetch_add(1,std::memory_order_relaxed);
        return zeroActualOutputs(data); // no native process and no owner RT callbacks
    }
    struct ProcessScope {ProcessScope(){++nativeProcessDepth;++hookDepth;}~ProcessScope(){--hookDepth;--nativeProcessDepth;}} processScope;
    auto* runtime=slot->runtime;
    if(data.inputParameterChanges&&callbacks(runtime)&&runtime->config.callbacks.inputParameterChangesObserved){
        // Standard host-owned VST3 realtime queue accessor only. Empty queue
        // objects are normal and must not discard every cached ARA block.
        const auto parameterCount=data.inputParameterChanges->getParameterCount();
        if(parameterCount>0)runtime->config.callbacks.inputParameterChangesObserved(instance,
            data.inputParameterChanges,parameterCount,threadInfo(),runtime->config.callbackContext);
    }
    if(callbacks(runtime)&&runtime->config.callbacks.beforeProcess)
        runtime->config.callbacks.beforeProcess(instance,data,threadInfo(),runtime->config.callbackContext);
    if(callbacks(runtime)&&runtime->config.callbacks.nativeProcessTelemetry){
        const NativeProcessTelemetry telemetry{NativeProcessTelemetryPhase::BeforeOriginal,
            static_cast<Vst::IAudioProcessor*>(self),&data,false,kInternalError};
        runtime->config.callbacks.nativeProcessTelemetry(instance,telemetry,threadInfo(),runtime->config.callbackContext);
    }
    const auto result=function(self,data);
    if(callbacks(runtime)&&runtime->config.callbacks.nativeProcessTelemetry){
        const NativeProcessTelemetry telemetry{NativeProcessTelemetryPhase::AfterOriginal,
            static_cast<Vst::IAudioProcessor*>(self),&data,true,result};
        runtime->config.callbacks.nativeProcessTelemetry(instance,telemetry,threadInfo(),runtime->config.callbackContext);
    }
    if(callbacks(runtime)&&runtime->config.callbacks.afterProcess)
        runtime->config.callbacks.afterProcess(instance,data,result,threadInfo(),runtime->config.callbackContext);
    return result;
}
uint32 PLUGIN_API processContextRequirements(void* self) {
    const auto actualNativeCaller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto* slot=slotFor(self);auto function=original<Requirements>(slot,3);
    if(bypass()||!actualObject(slot,self))return function(self);
    HookScope scope;slot->instance->contextRequirementCalls.fetch_add(1,std::memory_order_relaxed);
    const auto result=function(self);auto* runtime=slot->runtime;
    if(callbacks(runtime)&&runtime->config.callbacks.contextRequirementsObserved)
        runtime->config.callbacks.contextRequirementsObserved(*slot->instance,result,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    return result;
}
IPlugView* PLUGIN_API controllerCreateView(void* self,FIDString name) {
    const auto actualNativeCaller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto* slot=slotFor(self);auto function=original<IPlugView*(PLUGIN_API *)(void*,FIDString)>(slot,17);
    if(bypass()||!actualObject(slot,self))return function(self,name);
    HookScope scope;slot->instance->editorViewCalls.fetch_add(1,std::memory_order_relaxed);
    auto* result=function(self,name);auto* runtime=slot->runtime;
    if(callbacks(runtime)&&runtime->config.callbacks.viewCreated)
        runtime->config.callbacks.viewCreated(*slot->instance,name,result,threadInfo(actualNativeCaller),runtime->config.callbackContext);
    return result;
}
} // namespace

tresult OriginalStateCall::operator()(IBStream* stream)noexcept {
    if(calls){duplicate=true;return kInternalError;}++calls;
    if(!function)return result=kInternalError;
    try{return result=invokeOriginalState(function,self,stream);}catch(...){return result=kInternalError;}
}
InstallResult install(const Config& config)noexcept {
    if(activeRuntime.load(std::memory_order_acquire))return {false,"already installed; immutable process config"};
    if(nativeProcessDepth||bypass())return {false,"installation is not an owned native model boundary"};
    try{
        char executable[PATH_MAX];uint32_t capacity=sizeof(executable);
        if(config.exactOwnedProcessID!=getpid()||_NSGetExecutablePath(executable,&capacity)
            ||canonical(executable)!=config.exactOwnedExecutablePath||config.exactOwnedExecutableSHA256.size()!=64
            ||fileSHA(config.exactOwnedExecutablePath)!=config.exactOwnedExecutableSHA256)
            return {false,"exact owned process path/PID/SHA guard failed"};
        if(!config.confirmAlreadyAdmittedOwnedProcess||!config.confirmAlreadyAdmittedOwnedProcess(config.callbackContext))
            return {false,"owner has not admitted this actual FL process"};
        auto* runtime=new Runtime;runtime->config=config;
        Runtime* expected=nullptr;
        if(!activeRuntime.compare_exchange_strong(expected,runtime,std::memory_order_release,std::memory_order_acquire)){
            delete runtime;return {false,"concurrent install refused"};
        }
        return {true,"installed for exact admitted owned process"};
    }catch(...){return {false,"installation metadata unavailable"};}
}
void disableCallbacks()noexcept {
    if(auto* runtime=activeRuntime.load(std::memory_order_acquire))runtime->callbacksEnabled.store(false,std::memory_order_release);
}
bool installed()noexcept{return activeRuntime.load(std::memory_order_acquire)!=nullptr;}
InstanceRecord* findInstance(void* object)noexcept {
    auto* runtime=activeRuntime.load(std::memory_order_acquire);if(!runtime||nativeProcessDepth)return nullptr;
    std::lock_guard<std::mutex> lock(runtime->mutex);auto found=runtime->interfaces.find(object);
    return found==runtime->interfaces.end()?nullptr:found->second->instance;
}
FactoryRecord* findFactory(IPluginFactory* object)noexcept {
    auto* runtime=activeRuntime.load(std::memory_order_acquire);if(!runtime||nativeProcessDepth)return nullptr;
    std::lock_guard<std::mutex> lock(runtime->mutex);auto found=runtime->interfaces.find(object);
    return found==runtime->interfaces.end()?nullptr:found->second->factory;
}
tresult captureOriginalComponentState(InstanceRecord& instance,IBStream& destination)noexcept {
    // This function is an explicit own diagnostic. Do not route it through the
    // virtual getState, fabricate a native caller PC, or invoke owner state glue.
    auto* runtime=activeRuntime.load(std::memory_order_acquire);
    const auto admission=instance.nativeProcessAdmission.snapshot();
    if(!runtime||!pthread_main_np()||nativeProcessDepth||originalOnlyDiagnosticStateDepth||nativeOriginalStateMethodDepth
        ||!instance.nativeAlive.load(std::memory_order_acquire)
        ||!instance.nativeInitialized.load(std::memory_order_acquire)
        ||instance.nativeTerminated.load(std::memory_order_acquire)
        ||admission.allowed||admission.activeCount!=0)return kResultFalse;
    auto* component=instance.actualComponent.load(std::memory_order_acquire);
    if(!component)return kResultFalse;
    StateFunction function=nullptr;
    try{
        {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            auto found=runtime->interfaces.find(component);
            if(found==runtime->interfaces.end())return kResultFalse;
            const auto* slot=found->second;
            if(slot->kind!=Kind::Component||slot->instance!=&instance||slot->object!=component
                ||!slot->original[13]||__atomic_load_n(reinterpret_cast<void***>(component),__ATOMIC_ACQUIRE)!=slot->publishedTable)
                return kResultFalse;
            function=reinterpret_cast<StateFunction>(slot->original[13]);
        } // Never hold the registry lock across a vendor method.
        struct DiagnosticScope {
            DiagnosticScope()noexcept{++originalOnlyDiagnosticStateDepth;}
            ~DiagnosticScope(){--originalOnlyDiagnosticStateDepth;}
        } diagnostic;
        instance.originalOnlyDiagnosticGetStateCalls.fetch_add(1,std::memory_order_relaxed);
        return invokeOriginalState(function,component,&destination);
    }catch(...){return kInternalError;} // no retry or original-state fallback
}
} // namespace rysup::fl_ara::native_hooks

#define RYSUP_GENERIC_INTERPOSE(replacement,replacee) \
    __attribute__((used)) static struct {const void* replacement;const void* replacee;} \
    rysup_interpose_##replacement __attribute__((section("__DATA,__interpose"))) = { \
        reinterpret_cast<const void*>(&rysup::fl_ara::native_hooks::replacement),reinterpret_cast<const void*>(&replacee)}
RYSUP_GENERIC_INTERPOSE(observeCFBundleGetFunctionPointerForName,CFBundleGetFunctionPointerForName);
