#include "FLNativeSlotPowerObserver.h"
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace rysup::fl_ara::slot_power { namespace {
constexpr std::uintptr_t vmtOffset=0x2aaa70,slotOffset=0xd0,dispatcherOffset=0x64c60;
constexpr const char* sliceSHA="7ce89557800749bd30f525ebd316f8d47d5d1c5ff4eb3c38068d7a3972d61190";
constexpr const char* prologueSHA="89f3792036f494a752210e7c431d54d6676fcde4ac61705086052e81ae42f6bd";
constexpr const char* caseSHA="8f43d477f50c8dece569af7ccf8eba254f55d62b0fb232d80ed5fd3e3b628fa4";
static_assert(sizeof(Dispatcher)==sizeof(std::uintptr_t));
static_assert(std::atomic<Dispatcher>::is_always_lock_free);
static_assert(std::atomic<Registry<32>*>::is_always_lock_free);
struct Publication {
    std::atomic<Dispatcher> original{nullptr};
    std::atomic<Registry<32>*> registry{nullptr};
    std::atomic<unsigned> installClaim{0};
    std::atomic<bool> installed{false},protectionRestored{false};
    std::atomic<ObserverInstallReason> reason{ObserverInstallReason::NotAttempted};
    std::atomic<std::uint64_t> entries{0},inFlight{0},id11{0},normalReturns{0};
    // Intentionally never dlclose either retained already-loaded image.
    void* wrapperLease=nullptr;void* helperLease=nullptr;
    std::uintptr_t slot=0,originalAddress=0;
};
Publication publication;
struct EntryGuard {
    EntryGuard()noexcept{publication.entries.fetch_add(1,std::memory_order_relaxed);publication.inFlight.fetch_add(1,std::memory_order_relaxed);}
    ~EntryGuard(){publication.inFlight.fetch_sub(1,std::memory_order_relaxed);}
};
// Exact native scalar ABI. No vendor pointer reads, SDK, model/UI/IO calls,
// allocation, lock, wait or registry mutation here. No post-original lookup.
std::intptr_t dispatcherThunk(void* self,std::intptr_t id,std::intptr_t index,std::intptr_t value){
    EntryGuard entry;
    if(id==setEnabledID)publication.id11.fetch_add(1,std::memory_order_relaxed);
    auto* registry=publication.registry.load(std::memory_order_acquire);
    const auto original=publication.original.load(std::memory_order_acquire);
    // Both immutable process-lifetime pointers precede native slot publication.
    const auto result=forwardOriginalExactlyOnce(*registry,original,self,id,index,value);
    publication.normalReturns.fetch_add(1,std::memory_order_relaxed);return result;
}
std::string canonical(const char* path){char resolved[4096];return path&&realpath(path,resolved)?resolved:"";}
template<class T>T at(const void* data,std::size_t offset){T value{};std::memcpy(&value,static_cast<const std::uint8_t*>(data)+offset,sizeof(value));return value;}
std::uint32_t big32(const std::uint8_t* p){return std::uint32_t(p[0])<<24|std::uint32_t(p[1])<<16|std::uint32_t(p[2])<<8|p[3];}
std::uint64_t big64(const std::uint8_t* p){return std::uint64_t(big32(p))<<32|big32(p+4);}
std::string digest(const void* bytes,std::size_t size){
    if(size>std::numeric_limits<CC_LONG>::max())return{};
    std::array<unsigned char,32> hash{};CC_SHA256(bytes,static_cast<CC_LONG>(size),hash.data());
    static const char hex[]="0123456789abcdef";std::string result;result.reserve(64);
    for(auto byte:hash){result+=hex[byte>>4];result+=hex[byte&15];}return result;
}
bool read(std::uintptr_t address,void* destination,std::size_t bytes)noexcept{
    if(!address||!destination||!bytes||address>UINTPTR_MAX-bytes)return false;
    mach_vm_size_t copied=0;return mach_vm_read_overwrite(mach_task_self(),address,bytes,
        reinterpret_cast<mach_vm_address_t>(destination),&copied)==KERN_SUCCESS&&copied==bytes;
}
struct Image {
    std::array<std::uint8_t,16> uuid{};bool hasUUID=false,plainARM64=false;
    bool dispatcherExecutable=false,slotData=false;
};
bool parseImage(const std::vector<std::uint8_t>& bytes,Image& image){
    if(bytes.size()<sizeof(mach_header_64))return false;
    const auto header=at<mach_header_64>(bytes.data(),0);
    if(header.magic!=MH_MAGIC_64||header.cputype!=CPU_TYPE_ARM64||
        (header.cpusubtype&~CPU_SUBTYPE_MASK)!=CPU_SUBTYPE_ARM64_ALL||header.filetype!=MH_DYLIB||
        header.ncmds>4096||header.sizeofcmds>1024*1024||header.sizeofcmds>bytes.size()-sizeof(header))return false;
    image.plainARM64=true;std::size_t position=sizeof(header),end=position+header.sizeofcmds;
    for(std::uint32_t i=0;i<header.ncmds;++i){
        if(position>end||sizeof(load_command)>end-position)return false;
        const auto command=at<load_command>(bytes.data(),position);
        if(command.cmdsize<sizeof(command)||command.cmdsize>end-position||command.cmdsize%8)return false;
        if(command.cmd==LC_UUID){
            if(image.hasUUID||command.cmdsize!=sizeof(uuid_command))return false;
            const auto uuid=at<uuid_command>(bytes.data(),position);std::copy(std::begin(uuid.uuid),std::end(uuid.uuid),image.uuid.begin());image.hasUUID=true;
        }
        if(command.cmd==LC_SEGMENT_64){
            if(command.cmdsize<sizeof(segment_command_64))return false;
            const auto segment=at<segment_command_64>(bytes.data(),position);
            if(segment.vmaddr>UINT64_MAX-segment.vmsize)return false;
            const auto contains=[&](std::uint64_t address,std::size_t extent){return address>=segment.vmaddr&&address<=UINT64_MAX-extent&&address+extent<=segment.vmaddr+segment.vmsize;};
            if(contains(dispatcherOffset,0x50)&&(segment.initprot&VM_PROT_READ)&&(segment.initprot&VM_PROT_EXECUTE))image.dispatcherExecutable=true;
            if(contains(vmtOffset+slotOffset,sizeof(std::uintptr_t))&&(segment.initprot&VM_PROT_READ)&&!(segment.initprot&VM_PROT_EXECUTE))image.slotData=true;
        }
        position+=command.cmdsize;
    }
    return position==end&&image.hasUUID&&image.dispatcherExecutable&&image.slotData;
}
bool diskImage(const std::string& path,Image& image){
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    struct Close{int fd;~Close(){close(fd);}} close{fd};struct stat before{},after{};
    if(fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_size<32||before.st_size>256LL*1024*1024)return false;
    std::array<std::uint8_t,4096> prefix{};const auto got=pread(fd,prefix.data(),prefix.size(),0);if(got<32)return false;
    std::uint64_t offset=0,length=before.st_size;const auto magic=big32(prefix.data());
    if(magic==0xcafebabeU||magic==0xcafebabfU){
        const auto count=big32(prefix.data()+4);const bool wide=magic==0xcafebabfU;const std::size_t stride=wide?32:20;
        if(!count||count>32||8+count*stride>std::size_t(got))return false;
        unsigned found=0;for(std::uint32_t i=0;i<count;++i){const auto* row=prefix.data()+8+i*stride;
            if(big32(row)==CPU_TYPE_ARM64){++found;offset=wide?big64(row+8):big32(row+8);length=wide?big64(row+16):big32(row+12);}}
        if(found!=1)return false;
    }
    if(length<sizeof(mach_header_64)||offset>std::uint64_t(before.st_size)||length>std::uint64_t(before.st_size)-offset)return false;
    std::vector<std::uint8_t> slice(static_cast<std::size_t>(length));
    if(pread(fd,slice.data(),slice.size(),off_t(offset))!=ssize_t(slice.size())||digest(slice.data(),slice.size())!=sliceSHA||!parseImage(slice,image))return false;
    return !fstat(fd,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&
        before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec&&before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec;
}
bool loadedImage(std::uintptr_t base,Image& image){
    mach_header_64 header{};if(!read(base,&header,sizeof(header))||header.sizeofcmds>1024*1024)return false;
    std::vector<std::uint8_t> commands(sizeof(header)+header.sizeofcmds);
    return read(base,commands.data(),commands.size())&&parseImage(commands,image);
}
struct Region {std::uintptr_t start=0;std::uint64_t bytes=0;vm_prot_t protection=0;};
bool regionAt(std::uintptr_t location,std::size_t extent,Region& out)noexcept{
    if(!location||!extent||location>UINTPTR_MAX-extent)return false;
    mach_vm_address_t region=location;mach_vm_size_t bytes=0;vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t memory=MACH_PORT_NULL;
    const auto result=mach_vm_region(mach_task_self(),&region,&bytes,VM_REGION_BASIC_INFO_64,reinterpret_cast<vm_region_info_t>(&info),&count,&memory);
    if(memory!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),memory);
    if(result!=KERN_SUCCESS||region>location||region>UINTPTR_MAX-bytes||location+extent>region+bytes)return false;
    out={region,bytes,info.protection};return true;
}
bool instructionPin(std::uintptr_t base,std::uintptr_t offset,const char* expected){
    std::array<std::uint8_t,0x50> bytes{};Region region;
    return base<=UINTPTR_MAX-offset&&regionAt(base+offset,bytes.size(),region)&&
        (region.protection&VM_PROT_READ)&&(region.protection&VM_PROT_EXECUTE)&&!(region.protection&VM_PROT_WRITE)&&
        read(base+offset,bytes.data(),bytes.size())&&digest(bytes.data(),bytes.size())==expected;
}
bool exactImageAt(std::uintptr_t address,std::uintptr_t base,const std::string& path){
    Dl_info image{};return dladdr(reinterpret_cast<void*>(address),&image)&&image.dli_fbase&&image.dli_fname&&
        reinterpret_cast<std::uintptr_t>(image.dli_fbase)==base&&canonical(image.dli_fname)==path;
}
void* retainLoaded(std::uintptr_t address){
    Dl_info image{};if(!dladdr(reinterpret_cast<void*>(address),&image)||!image.dli_fbase||!image.dli_fname)return nullptr;
    const auto path=canonical(image.dli_fname);if(path.empty())return nullptr;
    void* lease=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);
    if(!lease&&image.dli_fbase==_dyld_get_image_header(0))lease=dlopen(nullptr,RTLD_NOW|RTLD_LOCAL);
    return lease;
}
struct PointerChange {bool changed=false,restored=false;};
PointerChange replaceDataPointer(std::uintptr_t slot,std::uintptr_t expected,std::uintptr_t replacement)noexcept{
    PointerChange result;Region region;
    if(slot%alignof(std::uintptr_t)||!regionAt(slot,sizeof(std::uintptr_t),region)||
        !(region.protection&VM_PROT_READ)||(region.protection&VM_PROT_EXECUTE)||!replacement)return result;
    const auto page=slot&~(std::uintptr_t(vm_page_size)-1);
    const bool writable=region.protection&VM_PROT_WRITE;
    if(!writable&&mach_vm_protect(mach_task_self(),page,vm_page_size,false,region.protection|VM_PROT_WRITE|VM_PROT_COPY)!=KERN_SUCCESS)return result;
    result.changed=__atomic_compare_exchange_n(reinterpret_cast<std::uintptr_t*>(slot),&expected,replacement,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    result.restored=writable||mach_vm_protect(mach_task_self(),page,vm_page_size,false,region.protection)==KERN_SUCCESS;
    return result;
}
} // namespace

ObserverInstallResult installLoadedNativeSlotPowerObserver(const ObserverConfig& config)noexcept{
    ObserverInstallResult result;unsigned expected=0;
    if(!publication.installClaim.compare_exchange_strong(expected,1)){result.reason=expected==2?ObserverInstallReason::AlreadyOwned:ObserverInstallReason::Busy;return result;}
    struct Claim {~Claim(){publication.installClaim.store(publication.installed.load()?2:0);}} claim;
    auto refuse=[&](ObserverInstallReason reason){result.reason=reason;publication.reason.store(reason);return result;};
    try{
#if defined(__arm64e__) || __has_feature(ptrauth_calls)
        return refuse(ObserverInstallReason::UnsupportedPointerABI);
#endif
        if(!config.registry||!config.actualWrapperImageBase||!config.confirmAdmittedModelBoundary)return refuse(ObserverInstallReason::Configuration);
        if(!config.confirmAdmittedModelBoundary(config.context))return refuse(ObserverInstallReason::Admission);
        const auto path=canonical(config.exactLoadedWrapperPath.c_str());
        if(path.empty()||path!=config.exactLoadedWrapperPath)return refuse(ObserverInstallReason::Path);
        const auto base=config.actualWrapperImageBase;
        if(base>UINTPTR_MAX-(vmtOffset+slotOffset+sizeof(std::uintptr_t)))return refuse(ObserverInstallReason::LoadedImage);
        Image disk,loaded;
        if(!diskImage(path,disk))return refuse(ObserverInstallReason::DiskImage);
        if(!exactImageAt(base+dispatcherOffset,base,path)||!loadedImage(base,loaded))return refuse(ObserverInstallReason::LoadedImage);
        if(disk.uuid!=loaded.uuid)return refuse(ObserverInstallReason::UUID);
        if(!instructionPin(base,dispatcherOffset,prologueSHA)||!instructionPin(base,0x65360,caseSHA))return refuse(ObserverInstallReason::InstructionPins);
        result.slot=base+vmtOffset+slotOffset;result.original=base+dispatcherOffset;std::uintptr_t actual=0;
        if(!exactImageAt(result.slot,base,path))return refuse(ObserverInstallReason::LoadedImage);
        if(!read(result.slot,&actual,sizeof(actual))||actual!=result.original)return refuse(ObserverInstallReason::OriginalPointer);
        Region data;if(!regionAt(result.slot,sizeof(std::uintptr_t),data)||!(data.protection&VM_PROT_READ)||(data.protection&VM_PROT_EXECUTE))return refuse(ObserverInstallReason::Page);
        const auto thunk=reinterpret_cast<std::uintptr_t>(&dispatcherThunk);
        Dl_info helper{},owner{};mach_header_64 helperHeader{};Region helperCode;
        if(!dladdr(reinterpret_cast<void*>(thunk),&helper)||!helper.dli_fbase||
            !dladdr(reinterpret_cast<void*>(config.confirmAdmittedModelBoundary),&owner)||owner.dli_fbase!=helper.dli_fbase||
            !read(reinterpret_cast<std::uintptr_t>(helper.dli_fbase),&helperHeader,sizeof(helperHeader))||
            helperHeader.magic!=MH_MAGIC_64||helperHeader.cputype!=CPU_TYPE_ARM64||
            (helperHeader.cpusubtype&~CPU_SUBTYPE_MASK)!=CPU_SUBTYPE_ARM64_ALL||
            !regionAt(thunk,1,helperCode)||!(helperCode.protection&VM_PROT_EXECUTE)||
            (helperCode.protection&VM_PROT_WRITE))return refuse(ObserverInstallReason::CodeLifetime);
        publication.wrapperLease=retainLoaded(result.original);publication.helperLease=retainLoaded(thunk);
        if(!publication.wrapperLease||!publication.helperLease)return refuse(ObserverInstallReason::CodeLifetime);
        if(!config.confirmAdmittedModelBoundary(config.context)||!exactImageAt(result.original,base,path))return refuse(ObserverInstallReason::Admission);
        publication.slot=result.slot;publication.originalAddress=result.original;
        publication.original.store(reinterpret_cast<Dispatcher>(result.original),std::memory_order_release);
        publication.registry.store(config.registry,std::memory_order_release);
        const auto change=replaceDataPointer(result.slot,result.original,thunk);
        if(!change.changed)return refuse(change.restored?ObserverInstallReason::Publication:ObserverInstallReason::Page);
        // Do not clear any state if restoration failed AFTER publication: native
        // callers can already be in our thunk. All retained code/storage stays.
        publication.installed.store(true);publication.protectionRestored.store(change.restored);
        result.installed=true;result.protectionRestored=change.restored;
        return refuse(change.restored?ObserverInstallReason::Installed:ObserverInstallReason::ProtectionRestore);
    }catch(...){return refuse(ObserverInstallReason::Exception);}
}
ObserverStats nativeSlotPowerObserverStats()noexcept{
    return {publication.installed.load(),publication.protectionRestored.load(),publication.entries.load(),publication.inFlight.load(),
        publication.id11.load(),publication.normalReturns.load(),publication.reason.load()};
}
} // namespace rysup::fl_ara::slot_power
