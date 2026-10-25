#include "FLStandaloneSourceOverlay.h"
#include "../host/StandaloneARACoordinator.h"
#include "../host/FLStandaloneGraphDecoder.h"
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace rysup::ara_lab {
namespace {
constexpr std::uintptr_t voiceVMTOffset=0x1554a58,sourceReadOffset=0x7da970;
constexpr std::uintptr_t sourceReadSlot=0x130,channelVMTOffset=0x1508640;
// Same exact ARM64 engine: passive retained/current R6 proof found this class
// (2288/-2288 bytes) deriving directly from 0x1508640 (2016/-2016 bytes).
// FLNativeModel validates the bounded FPC parent chain and retains ACTUAL vptr.
// This is one observed derived class, not admission of arbitrary subclasses.
constexpr std::uintptr_t observedAudioChannelVMTOffset=0x150c000;
// Exact same sealed engine: init +0x30c274 constructs TPreviewChannel (VMT
// +0x150c280), then +0x30c278..280 stores it at +0x1e36700. Native RenderSnd
// playback uses this independent singleton, not the Playlist channel manager.
constexpr std::uintptr_t previewChannelVMTOffset=0x150c280,globalPreviewChannelOffset=0x1e36700;
constexpr std::uintptr_t sampleVMTOffset=0x146bb98,baseSampleVMTOffset=0x144f960;
constexpr std::uintptr_t readerVMTOffset=0x1487358,blockFramesOffset=0x21a23b4;
constexpr std::size_t maximumInstances=64,maximumRegions=100000,maximumChannels=4096;
constexpr std::size_t maximumPCMBytes=128u*1024u*1024u,maximumCacheBytes=256u*1024u*1024u;
constexpr std::int32_t maximumBlockFrames=65536;
static_assert(std::atomic<void*>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::is_trivially_copyable<FLStandaloneSourceRead>::value);
static_assert(std::is_trivially_copyable<FLStandalonePhysicalIdentityDiagnostic>::value);
static_assert(kFLStandalonePhysicalIdentityDiagnosticSlots==4);
using Digest=std::array<std::uint8_t,32>;
using Record=std::array<std::uint8_t,88>;

void demand(bool good,const char* reason){if(!good)throw std::runtime_error(reason);}
bool validSHA(const std::string& value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
std::string sha(const void* data,std::size_t bytes){
    demand(bytes<=std::numeric_limits<CC_LONG>::max(),"native overlay SHA extent exceeds bound");
    Digest digest{};CC_SHA256(data,static_cast<CC_LONG>(bytes),digest.data());
    static constexpr char digits[]="0123456789abcdef";
    std::string text; text.reserve(64);for(auto b:digest){text+=digits[b>>4];text+=digits[b&15];}return text;
}
Digest parseDigest(const std::string& text){
    demand(validSHA(text),"native overlay PCM SHA missing");Digest digest{};
    auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(std::size_t i=0;i<digest.size();++i)digest[i]=std::uint8_t(digit(text[i*2])*16+digit(text[i*2+1]));return digest;
}
bool capturedChannelClassQualified(const FLNativeModelSnapshot& model,const FLNativeChannelSnapshot& channel,std::uintptr_t base){
    if(base>std::numeric_limits<std::uintptr_t>::max()-observedAudioChannelVMTOffset)return false;
    if(channel.actualChannelVMT==base+channelVMTOffset)return true; // Existing exact base admission.
    if(channel.actualChannelVMT!=base+observedAudioChannelVMTOffset)return false;
    // The observed derived class additionally requires the complete actual
    // reader inventory, whose class was already proven by validateObject's
    // sealed-image FPC ancestry walk. Never substitute/normalize its vptr.
    if(!model.repeatedReadsMatched||model.verifiedArm64SliceSHA256!=kFLStandaloneOverlayEngineArm64SHA256||model.engineImageBase!=base
        ||!model.allNativeManagerChannelsInspected||model.nativeChannelCount<0||model.nativeManagerMembers.size()!=std::size_t(model.nativeChannelCount)
        ||channel.managerIndex>=model.nativeManagerMembers.size())return false;
    const auto& member=model.nativeManagerMembers[channel.managerIndex];
    return member.managerIndex==channel.managerIndex&&member.actualChannelObject==channel.channelObject
        &&member.actualChannelVMT==channel.actualChannelVMT&&member.nativeChannelType150==4&&channel.nativeChannelType150==4;
}
std::string physicalChannelRefusal(const FLNativeModelSnapshot& model,const FLStandaloneSourceEvidence& source,
    const FLNativeChannelSnapshot* channel,std::uintptr_t base,std::int32_t mixer){
    // Model-thread diagnostic only. RT checks below retain fixed enum/atomic
    // reporting and perform no allocation, ancestry walk or native call.
    std::ostringstream text;text<<"standalone source actual physical native channel differs: evidence_channel="<<source.nativeChannelIndex
        <<" channel_found="<<(channel?1:0)<<" model_sources="<<model.sources.size()<<" expected_mixer="<<mixer
        <<" engine_base=0x"<<std::hex<<base<<" expected_base_vmt=0x"<<(base+channelVMTOffset)
        <<" expected_observed_derived_vmt=0x"<<(base+observedAudioChannelVMTOffset)<<std::dec;
    if(channel){text<<" manager_index="<<channel->managerIndex<<" source_index="<<channel->sourceSnapshotIndex<<" type150="<<channel->nativeChannelType150
        <<" mixer_supported="<<channel->nativeMixerInsertSupported<<" actual_mixer="<<channel->nativeMixerInsert248
        <<" channel_object=0x"<<std::hex<<channel->channelObject<<" actual_vmt=0x"<<channel->actualChannelVMT;
        if(channel->actualChannelVMT>=base)text<<" actual_vmt_offset=0x"<<(channel->actualChannelVMT-base);
        text<<std::dec<<" complete_reader_inventory="<<model.allNativeManagerChannelsInspected
            <<" captured_class_qualified="<<capturedChannelClassQualified(model,*channel,base);}
    return text.str();
}

template<class T>T fromBytes(const void* bytes,std::size_t offset)noexcept{T value{};std::memcpy(&value,static_cast<const std::uint8_t*>(bytes)+offset,sizeof(value));return value;}
bool boundedRead(std::uintptr_t source,void* destination,std::size_t bytes)noexcept{
    if(!source||!destination||!bytes||source>std::numeric_limits<std::uintptr_t>::max()-bytes)return false;
    mach_vm_size_t copied=0;
    return mach_vm_read_overwrite(mach_task_self(),source,bytes,reinterpret_cast<mach_vm_address_t>(destination),&copied)==KERN_SUCCESS&&copied==bytes;
}
template<class T>bool readAt(std::uintptr_t source,std::uintptr_t offset,T& value)noexcept{
    return source<=std::numeric_limits<std::uintptr_t>::max()-offset&&boundedRead(source+offset,&value,sizeof(value));
}
void recordDefaultPreviewOwnerObservation(FLStandaloneSourceRead& read,bool beforeOK,std::uintptr_t before,
    bool afterOK,std::uintptr_t after)noexcept{
    read.globalPreviewChannelObject=afterOK?after:0;
    read.globalPreviewOwnerReadCoherent=beforeOK&&afterOK&&before&&before==after;
}
std::string canonical(const std::string& path){char buffer[4096];return !path.empty()&&realpath(path.c_str(),buffer)?buffer:"";}
std::uint32_t big32(const std::uint8_t* p){return std::uint32_t(p[0])<<24|std::uint32_t(p[1])<<16|std::uint32_t(p[2])<<8|p[3];}
std::uint64_t big64(const std::uint8_t* p){return std::uint64_t(big32(p))<<32|big32(p+4);}
std::string stableArm64SliceSHA(const std::string& path){
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);demand(fd>=0,"native overlay cannot read admitted engine file");
    struct Close{int fd;~Close(){close(fd);}}closeFile{fd};struct stat before{},after{};
    demand(!fstat(fd,&before)&&S_ISREG(before.st_mode)&&before.st_size>=32&&before.st_size<=256LL*1024*1024,"native overlay engine file extent unsupported");
    std::array<std::uint8_t,4096> header{};const auto got=pread(fd,header.data(),header.size(),0);demand(got>=32,"native overlay engine file header unreadable");
    std::uint64_t start=0,length=std::uint64_t(before.st_size);const auto magic=big32(header.data());
    if(magic==0xcafebabeU||magic==0xcafebabfU){
        const auto count=big32(header.data()+4);const bool wide=magic==0xcafebabfU;const std::size_t stride=wide?32:20;
        demand(count>0&&count<=32&&8+std::size_t(count)*stride<=std::size_t(got),"native overlay fat engine header unsupported");
        unsigned matches=0;
        for(std::uint32_t i=0;i<count;++i){const auto* row=header.data()+8+i*stride;
            if(big32(row)==0x0100000cU){++matches;start=wide?big64(row+8):big32(row+8);length=wide?big64(row+16):big32(row+12);}}
        demand(matches==1&&length>=32&&start<=std::uint64_t(before.st_size)&&length<=std::uint64_t(before.st_size)-start,"native overlay requires one exact ARM64 engine slice");
    }
    std::array<std::uint8_t,32> thin{};demand(pread(fd,thin.data(),thin.size(),off_t(start))==ssize_t(thin.size())&&
        fromBytes<std::uint32_t>(thin.data(),0)==MH_MAGIC_64&&fromBytes<std::uint32_t>(thin.data(),4)==0x0100000cU,"native overlay engine slice is not native ARM64 Mach-O");
    CC_SHA256_CTX context;CC_SHA256_Init(&context);std::array<std::uint8_t,65536> buffer{};
    for(std::uint64_t offset=0;offset<length;){const auto n=std::min<std::uint64_t>(buffer.size(),length-offset);
        demand(pread(fd,buffer.data(),std::size_t(n),off_t(start+offset))==ssize_t(n),"native overlay engine slice changed while reading");
        CC_SHA256_Update(&context,buffer.data(),static_cast<CC_LONG>(n));offset+=n;}
    demand(!fstat(fd,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&
        before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec&&before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec,"native overlay engine file changed during verification");
    Digest digest{};CC_SHA256_Final(digest.data(),&context);static constexpr char digits[]="0123456789abcdef";std::string text;text.reserve(64);
    for(auto b:digest){text+=digits[b>>4];text+=digits[b&15];}return text;
}
struct PointerChange{bool changed=false,protectionRestored=false;};
PointerChange replaceCOWDataPointer(std::uintptr_t location,std::uintptr_t expected,std::uintptr_t replacement)noexcept{
    PointerChange out;mach_vm_address_t region=location;mach_vm_size_t bytes=0;vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t memory=MACH_PORT_NULL;
    if(mach_vm_region(mach_task_self(),&region,&bytes,VM_REGION_BASIC_INFO_64,reinterpret_cast<vm_region_info_t>(&info),&count,&memory)!=KERN_SUCCESS)return out;
    if(memory!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),memory);
    if(location%alignof(std::uintptr_t)||location<region||location>std::numeric_limits<std::uintptr_t>::max()-sizeof(std::uintptr_t)||
       location+sizeof(std::uintptr_t)>region+bytes||!(info.protection&VM_PROT_READ)||(info.protection&VM_PROT_EXECUTE))return out;
    const auto page=location&~(std::uintptr_t(vm_page_size)-1);const bool writable=info.protection&VM_PROT_WRITE;
    if(!writable&&mach_vm_protect(mach_task_self(),page,vm_page_size,false,info.protection|VM_PROT_WRITE|VM_PROT_COPY)!=KERN_SUCCESS)return out;
    out.changed=__atomic_compare_exchange_n(reinterpret_cast<std::uintptr_t*>(location),&expected,replacement,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    out.protectionRestored=writable||mach_vm_protect(mach_task_self(),page,vm_page_size,false,info.protection)==KERN_SUCCESS;return out;
}
struct AtomicCounters{
    std::atomic<std::uint64_t> calls{0},unrelated{0},stale{0},applied{0},frames{0},zero{0},rejected{0},bits{0};
    std::atomic<std::uint64_t> held{0},heldFrames{0},holdRequests{0};
    FLStandaloneOverlayCounters snapshot(std::uint32_t readers=0)const noexcept{
        FLStandaloneOverlayCounters result{calls.load(),unrelated.load(),stale.load(),applied.load(),frames.load(),zero.load(),rejected.load(),bits.load(),readers};
        result.heldCallbacks=held.load();result.heldFrames=heldFrames.load();result.staleHoldRequests=holdRequests.load();return result;
    }
};
struct HookPublication{
    std::atomic<FLStandaloneSourceOverlay*> router{nullptr};
    std::atomic<FLStandaloneNativeVoiceRead> original{nullptr};
    std::atomic<std::uintptr_t> engineBase{0};
    std::atomic<std::uint32_t> readers{0};
};
HookPublication hook;
struct ReaderGuard{std::atomic<std::uint32_t>& value;explicit ReaderGuard(std::atomic<std::uint32_t>& n)noexcept:value(n){value.fetch_add(1,std::memory_order_seq_cst);}~ReaderGuard(){value.fetch_sub(1,std::memory_order_seq_cst);}};
template<class After>void forwardOnce(FLStandaloneNativeVoiceRead original,void* self,std::uintptr_t enabled,void* context,float** output,
        std::int32_t* frames,std::uint8_t* ended,std::uint8_t* changed,After after){
    original(self,enabled,context,output,frames,ended,changed);after();
}
} // namespace

struct FLStandaloneOverlayNativeThunk{
    static void read(void* self,std::uintptr_t enabled,void* context,float** output,
            std::int32_t* frames,std::uint8_t* ended,std::uint8_t* changed){
        ReaderGuard guard(hook.readers);auto* router=hook.router.load(std::memory_order_seq_cst);
        const auto original=hook.original.load(std::memory_order_seq_cst);
        // Publication occurs only after original has been verified and stored.
        if(!original)std::terminate();
        double pre=0;std::uintptr_t reader=0,vmt=0;const auto voice=reinterpret_cast<std::uintptr_t>(self);
        const bool preRead=router&&readAt(voice,0,vmt)&&vmt==hook.engineBase.load(std::memory_order_seq_cst)+voiceVMTOffset&&
            readAt(voice,0x300,reader)&&readAt(reader,8,pre);
        forwardOnce(original,self,enabled,context,output,frames,ended,changed,[&]{
            if(router)router->processNativeAfterOriginal(self,context,output,frames,ended,changed,pre,preRead);
        });
    }
};

struct FLStandaloneSourceOverlay::Impl{
    struct PhysicalDiagnosticSlot{
        // One writer ever. 0 empty -> 1 exclusively writing -> 2 immutable ready.
        // Never reset/reused; ready readers cannot race with a later overwrite.
        std::atomic<std::uint8_t> state{0};
        FLStandalonePhysicalIdentityDiagnostic value;
    };
    struct Control{
        std::string id;std::uintptr_t component=0;AtomicCounters counters;
        std::atomic<std::uint64_t> pendingBits{0},faultCaptureSerial{0};
        std::atomic<std::uint8_t> firstFault{0};
        std::array<PhysicalDiagnosticSlot,kFLStandalonePhysicalIdentityDiagnosticSlots> physicalDiagnostics;
        std::atomic<bool> physicalDiagnosticOverflow{false};
        std::atomic<bool> holdDelivery{false};
        std::atomic<std::uint64_t> holdEpisodeGeneration{0},activePublicationGeneration{0};
    };
    struct Source{
        std::string sourceID,modificationID;Digest pcmSHA{};
        std::uint16_t channelIndex=0;std::uintptr_t channel=0,sample=0,pcm=0,channelVMT=0,sampleVMT=0;
        std::int64_t frames=0;std::uint32_t channels=0;
        const WholeModificationSourceCache* cache=nullptr;
        FLStandalonePhysicalIdentityExpected physicalExpected;

    };
    struct Region{std::int32_t id=-1;Record record{};std::size_t sourceIndex=0;};
    struct Batch{
        std::shared_ptr<Control> control;std::shared_ptr<const FLNativeModelSnapshot> model;
        StandaloneSourceCacheBatch cache;std::int32_t mixer=-1;
        std::size_t preparedCacheBytes=0;
        std::uint64_t publicationGeneration=0;
        std::vector<Source> sources;std::vector<Region> regions;
    };
    struct Table{
        struct Entry{const Batch* batch=nullptr;const Region* region=nullptr;const Source* source=nullptr;};
        struct Channel{const Batch* batch=nullptr;const Source* source=nullptr;};
        std::vector<std::shared_ptr<const Batch>> batches;
        std::vector<Entry> regions;
        std::vector<Channel> channelsByPointer,channelsByIndex;
    };
    FLStandaloneOverlayConfig config;std::thread::id modelThread=std::this_thread::get_id();
    std::map<std::string,std::shared_ptr<const Batch>> live;
    std::map<std::string,std::shared_ptr<Control>> controls;
    std::unique_ptr<Table> current;std::vector<std::unique_ptr<Table>> retired;
    std::atomic<Table*> table{nullptr};std::atomic<std::uint32_t> readers{0};AtomicCounters totals;
    std::atomic<std::uint64_t> qualifiedPreviewUnrelatedCallbacks{0};
    std::uintptr_t installedSlot=0;bool installed=false,shutDown=false;
    explicit Impl(FLStandaloneOverlayConfig c):config(std::move(c)){
        demand(config.actualEngineImageBase&&config.exactEngineArm64SliceSHA256==kFLStandaloneOverlayEngineArm64SHA256&&
            !config.exactLoadedEnginePath.empty()&&config.confirmAlreadyAdmittedOwnedProcess&&
            config.confirmStoppedDrainedNativeModelBoundary&&config.ownerFaultQueued&&config.ownerStaleHoldQueued,"standalone source router requires exact engine, admitted owner and fixed fault/stale-hold mailboxes");
        requireMain();demand(config.confirmAlreadyAdmittedOwnedProcess(config.ownerContext),"standalone source router owner was not admitted");
    }
    void requireMain()const{demand(std::this_thread::get_id()==modelThread,"standalone source router requires its owning model thread");}
    void requireAdmittedModelMaintenance()const{
        requireMain();demand(!shutDown&&config.confirmAlreadyAdmittedOwnedProcess(config.ownerContext),
            "standalone source table maintenance requires the same admitted process/model thread");
    }
    void requireBoundary()const{
        requireMain();demand(!shutDown&&config.confirmAlreadyAdmittedOwnedProcess(config.ownerContext)&&
            config.confirmStoppedDrainedNativeModelBoundary(config.ownerContext)&&hook.readers.load(std::memory_order_seq_cst)==0&&
            readers.load(std::memory_order_seq_cst)==0,"standalone source publication requires actual stopped/drained native owner boundary");
    }
    std::shared_ptr<const Batch> build(const std::string& id,const FLStandaloneDecodedGraph& decoded,
            std::shared_ptr<const FLNativeModelSnapshot> model,StandaloneSourceCacheBatch caches){
        demand(!id.empty()&&id.size()<=512&&caches.instanceID==id&&model&&caches.gate&&caches.captureSerial&&
            caches.gate->authorizes(caches.ticket),"standalone source batch identity/ticket/model absent or stale");
        demand(decoded.actualCurrentNativeRAMCopiedAndVerified&&decoded.noNativeSerializationOrFileAudioFallbackUsed&&
            decoded.actualAssociation.nativePointerAssociationVerified&&decoded.actualAssociation.actualComponent&&decoded.actualAssociation.nativeOwnerObject&&
            !decoded.actualAssociation.exactObjectAssociationEvidence.empty()&&decoded.actualAssociation.mixerInsert>=0&&decoded.actualAssociation.mixerInsert<=500,
            "standalone source decoder lacks actual native insert/model authority");
        demand(model->engineImageBase==config.actualEngineImageBase&&model->verifiedArm64SliceSHA256==config.exactEngineArm64SliceSHA256&&
            model->loadedImagePath==config.exactLoadedEnginePath&&model->repeatedReadsMatched&&model->nativeRecordStride==88&&
            model->nativeRecordCount>=0&&std::size_t(model->nativeRecordCount)==model->clips.size()&&model->clips.size()<=maximumRegions&&
            model->currentArrangementIndex==decoded.nativeArrangementIndex&&decoded.clock.engineImageBase==model->engineImageBase&&
            decoded.clock.engineArm64SliceSHA256==model->verifiedArm64SliceSHA256,"standalone source current native engine/record model differs");
        demand(caches.native.nativeEngineSHA256==config.exactEngineArm64SliceSHA256&&
            caches.native.nativeRecordModelSHA256==decoded.nativeRecordModelSHA256&&caches.native.sourcePCMModelSHA256==decoded.sourcePCMModelSHA256&&
            caches.native.graphSHA256==decoded.graphSHA256&&caches.native.nativeSaveVersion==decoded.nativeSaveVersion&&
            validSHA(decoded.nativeRecordModelSHA256)&&validSHA(decoded.sourcePCMModelSHA256)&&validSHA(decoded.graphSHA256)&&
            caches.native.clock.sampleRate==48000&&caches.native.clock.ppq==decoded.clock.ppq&&caches.native.clock.tempoMilliBPM==decoded.clock.tempoMilliBPM&&
            caches.native.clock.actualNativeClockBytes.size()==decoded.clock.actualClockBytes.size()&&
            std::equal(caches.native.clock.actualNativeClockBytes.begin(),caches.native.clock.actualNativeClockBytes.end(),decoded.clock.actualClockBytes.begin()),
            "standalone source cache does not belong to this exact decoded native model/clock");
        demand(decoded.actualCopiedNativeModelRecords.size()==model->clips.size()*88,"standalone source current native record extent differs");
        for(std::size_t i=0;i<model->clips.size();++i)demand(model->clips[i].nativeListIndex==i&&
            std::memcmp(model->clips[i].record.data(),decoded.actualCopiedNativeModelRecords.data()+i*88,88)==0,"standalone source copied native event block differs");
        demand(!decoded.sources.empty()&&decoded.sources.size()<=maximumChannels&&decoded.sources.size()==decoded.modifications.size()&&
            caches.modifications.size()==decoded.modifications.size()&&!decoded.regions.empty()&&decoded.regions.size()<=maximumRegions,
            "standalone source cache/source/modification/region cardinality differs");
        auto next=std::make_shared<Batch>();next->model=std::move(model);next->cache=std::move(caches);next->mixer=decoded.actualAssociation.mixerInsert;
        auto control=controls.find(id);
        if(control!=controls.end()){
            demand(control->second->component==decoded.actualAssociation.actualComponent&&!control->second->pendingBits.load(),
                "standalone source instance owner changed or fault is unacknowledged");next->control=control->second;
        }else{next->control=std::make_shared<Control>();next->control->id=id;next->control->component=decoded.actualAssociation.actualComponent;}
        std::map<std::string,const WholeModificationSourceCache*> cacheMap;
        for(const auto& cache:next->cache.modifications)demand(cacheMap.emplace(cache.modificationID,&cache).second,"standalone source duplicate modification cache");
        std::map<std::string,std::size_t> sourceMap;std::size_t pcmBytes=0,cacheBytes=0;
        next->sources.reserve(decoded.sources.size());
        for(const auto& evidence:decoded.sources){
            const auto channel=std::find_if(next->model->channels.begin(),next->model->channels.end(),[&](const auto& c){return c.managerIndex==evidence.nativeChannelIndex;});
            const bool physicalChannelMatches=channel!=next->model->channels.end()&&channel->sourceSnapshotIndex<next->model->sources.size()&&
                channel->nativeChannelType150==4&&channel->nativeMixerInsertSupported&&channel->nativeMixerInsert248==next->mixer&&
                channel->channelObject&&capturedChannelClassQualified(*next->model,*channel,config.actualEngineImageBase);
            if(!physicalChannelMatches)throw std::runtime_error(physicalChannelRefusal(*next->model,evidence,
                channel==next->model->channels.end()?nullptr:&*channel,config.actualEngineImageBase,next->mixer));
            const auto& sample=next->model->sources[channel->sourceSnapshotIndex];
            demand(sample.waveSampleObject&&sample.nativePCMAddress90&&channel->nativeWaveSampleObject328==sample.waveSampleObject&&
                channel->actualNativeWaveSampleVMT==sample.actualWaveSampleVMT&&
                (sample.actualWaveSampleVMT==config.actualEngineImageBase+sampleVMTOffset||sample.actualWaveSampleVMT==config.actualEngineImageBase+baseSampleVMTOffset)&&
                sample.qualifiedEncoding==FLNativePCMEncoding::SignedInt16LE&&(sample.qualifiedChannelCount==1||sample.qualifiedChannelCount==2)&&
                sample.nativeFormatByte78==sample.qualifiedChannelCount&&sample.nativeFormatByte79==0&&!sample.nativeReferenceC8&&
                sample.nativeRate88==48000&&sample.nativeRateFactor1A0==1&&sample.nativeFrameCount98>0&&!sample.pcmQualificationEvidenceID.empty(),
                "standalone source PCM class/format/rate/residency is unqualified");
            const auto frames=std::uint64_t(sample.nativeFrameCount98);const auto channels=sample.qualifiedChannelCount;
            demand(frames<=maximumPCMBytes/(channels*2)&&sample.pcmBytes.size()==frames*channels*2&&
                sample.pcmBytes.size()<=maximumPCMBytes-pcmBytes,"standalone source immutable PCM extent/budget differs");pcmBytes+=sample.pcmBytes.size();
            const auto digest=sha(sample.pcmBytes.data(),sample.pcmBytes.size());
            const auto sourceID="fl:native-channel:"+std::to_string(channel->managerIndex)+":pcm:"+digest;
            const auto modificationID=sourceID+":modification";
            demand(evidence.sourceID==sourceID&&evidence.actualPCM_SHA256==digest&&evidence.nativeMixerInsert==next->mixer&&
                evidence.sampleCount==sample.nativeFrameCount98&&evidence.channelCount==channels&&evidence.sampleRate==48000&&evidence.rateFactor==1&&
                evidence.formatByte78==channels&&evidence.formatByte79==0,"standalone source cached PCM hash/qualified metadata differs");
            const auto mod=std::find_if(decoded.modifications.begin(),decoded.modifications.end(),[&](const auto& m){return m.sourceID==sourceID;});
            demand(mod!=decoded.modifications.end()&&mod->modificationID==modificationID&&cacheMap.count(modificationID),"standalone source physical channel does not own its exact shared modification");
            const auto* cache=cacheMap.at(modificationID);
            demand(cache->source.id==sourceID&&cache->modificationID==modificationID&&cache->source.sampleCount==sample.nativeFrameCount98&&
                cache->source.channelCount==int(channels)&&cache->source.sampleRate==48000&&cache->channels.size()==channels&&cache->generation&&
                cache->actualDocumentController&&cache->actualAudioSource&&cache->actualAudioModification&&cache->actualOwnedPlaybackComponent&&
                cache->editNotificationSerial==next->cache.actualEditNotificationRevision&&cache->internalRegionRemoved&&
                cache->reportedLatencySamples==0&&cache->reportedHeadSeconds==0&&cache->reportedTailSeconds==0,"standalone source cache generation/ARA object/geometry is unsupported");
            for(const auto& values:cache->channels){demand(values.size()==frames&&values.size()<=(maximumCacheBytes-cacheBytes)/sizeof(float),"standalone source float cache extent/budget differs");
                cacheBytes+=values.size()*sizeof(float);for(auto value:values)demand(std::isfinite(value),"standalone source cache has nonfinite audio");}
            Source source;source.sourceID=sourceID;source.modificationID=modificationID;source.pcmSHA=parseDigest(digest);source.channelIndex=channel->managerIndex;
            source.channel=channel->channelObject;source.sample=sample.waveSampleObject;source.pcm=sample.nativePCMAddress90;source.channelVMT=channel->actualChannelVMT;
            source.sampleVMT=sample.actualWaveSampleVMT;source.frames=sample.nativeFrameCount98;source.channels=channels;source.cache=cache;
            // Immutable diagnostic provenance prepared OFF RT. RT copies these
            // values only; no later native/model-manager dereference is needed.
            auto& expected=source.physicalExpected;expected.nativeChannelIndex=source.channelIndex;expected.nativeMixerInsert=next->mixer;
            expected.channelObject=source.channel;expected.channelVMT=source.channelVMT;expected.sampleObject=source.sample;expected.sampleVMT=source.sampleVMT;
            expected.pcmAddress=source.pcm;expected.sourceFrames=source.frames;expected.sourceChannels=source.channels;
            expected.capturedModelAddress=reinterpret_cast<std::uintptr_t>(next->model.get());expected.capturedEngineBase=next->model->engineImageBase;
            expected.capturedChannelManager=next->model->nativeChannelManager;expected.capturedManagerPointerArray=next->model->nativeChannelPointerArray;
            expected.capturedManagerChannelCount=next->model->nativeChannelCount;
            expected.capturedChannelSnapshotIndex=static_cast<std::uint64_t>(std::distance(next->model->channels.begin(),channel));
            expected.capturedSourceSnapshotIndex=channel->sourceSnapshotIndex;expected.capturedChannelObject=channel->channelObject;
            expected.capturedChannelVMT=channel->actualChannelVMT;expected.capturedChannelType150=channel->nativeChannelType150;expected.capturedMixerInsert248=channel->nativeMixerInsert248;
            expected.completeManagerInspection=next->model->allNativeManagerChannelsInspected;
            if(channel->managerIndex<next->model->nativeManagerMembers.size()){
                const auto& member=next->model->nativeManagerMembers[channel->managerIndex];expected.managerMemberPresent=true;
                expected.capturedManagerMemberIndex=member.managerIndex;expected.capturedManagerMemberObject=member.actualChannelObject;
                expected.capturedManagerMemberVMT=member.actualChannelVMT;expected.capturedManagerMemberType150=member.nativeChannelType150;
            }
            demand(sourceMap.emplace(sourceID,next->sources.size()).second,"standalone source duplicate physical source mapping");next->sources.push_back(std::move(source));
        }
        std::set<std::int32_t> clipIDs;next->regions.reserve(decoded.regions.size());
        for(const auto& evidence:decoded.regions){
            demand(evidence.nativeClipID>0&&clipIDs.insert(evidence.nativeClipID).second&&sourceMap.count(evidence.sourceID)&&
                evidence.nativeListIndex<next->model->clips.size(),"standalone source actual region identity/mapping missing or duplicated");
            const auto& clip=next->model->clips[evidence.nativeListIndex];const auto index=sourceMap.at(evidence.sourceID);const auto& source=next->sources[index];
            const auto expectedRegion="fl:native-clip:"+std::to_string(clip.nativeClipID20)+":"+source.sourceID+":region";
            demand(clip.kind==FLNativeRecordKind::AudioChannel&&clip.nativeClipID20==evidence.nativeClipID&&clip.record==evidence.actualRecord&&
                clip.channelIndex06==source.channelIndex&&evidence.nativeChannelIndex==source.channelIndex&&evidence.modificationID==source.modificationID&&
                evidence.regionID==expectedRegion&&fromBytes<std::int32_t>(clip.record.data(),0x20)==clip.nativeClipID20&&
                fromBytes<std::uint16_t>(clip.record.data(),6)==source.channelIndex,"standalone source native88/region/source relation differs");
            next->regions.push_back({clip.nativeClipID20,clip.record,index});
        }
        // Every admitted current native audio row on this insert must have one
        // explicit region. New/duplicate clips are not silently source-mapped.
        for(const auto& clip:next->model->clips)if(clip.kind==FLNativeRecordKind::AudioChannel){
            demand(clip.channelSnapshotIndex<next->model->channels.size(),"standalone source current audio row channel missing");
            if(next->model->channels[clip.channelSnapshotIndex].nativeMixerInsert248==next->mixer)
                demand(clipIDs.count(clip.nativeClipID20),"standalone source current insert has an uncaptured audio region");
        }
        next->preparedCacheBytes=cacheBytes;
        const auto previousGeneration=next->control->activePublicationGeneration.load();
        demand(previousGeneration<std::numeric_limits<std::uint64_t>::max(),"standalone source publication generation exhausted");
        next->publicationGeneration=previousGeneration+1;return next;
    }
    std::unique_ptr<Table> makeTable(const std::map<std::string,std::shared_ptr<const Batch>>& instances){
        demand(instances.size()<=maximumInstances,"standalone source insert count exceeds fixed bound");auto next=std::make_unique<Table>();std::set<std::int32_t> mixers,clips;
        std::set<std::uintptr_t> physicalChannels;std::set<std::uint16_t> channelIndices;
        std::set<const FLNativeModelSnapshot*> copiedModels;std::size_t tableCacheBytes=0,tablePCMBytes=0;
        for(const auto& item:instances){const auto& b=item.second;demand(mixers.insert(b->mixer).second,"standalone source multiple ARA instances own the same mixer insert");
            demand(b->preparedCacheBytes<=maximumCacheBytes-tableCacheBytes,"standalone source combined insert caches exceed the memory bound");tableCacheBytes+=b->preparedCacheBytes;
            if(copiedModels.insert(b->model.get()).second)for(const auto& source:b->model->sources){
                demand(source.pcmBytes.size()<=maximumPCMBytes-tablePCMBytes,"standalone source combined immutable native models exceed the memory bound");tablePCMBytes+=source.pcmBytes.size();}
            next->batches.push_back(b);for(const auto& s:b->sources){demand(physicalChannels.insert(s.channel).second&&channelIndices.insert(s.channelIndex).second,"standalone source physical channel has ambiguous ARA ownership");
                next->channelsByPointer.push_back({b.get(),&s});next->channelsByIndex.push_back({b.get(),&s});}
            for(const auto& r:b->regions){demand(clips.insert(r.id).second,"standalone source native clip has ambiguous ARA ownership");next->regions.push_back({b.get(),&r,&b->sources[r.sourceIndex]});}}
        demand(next->regions.size()<=maximumRegions&&next->channelsByPointer.size()<=maximumChannels,"standalone source prepared region/channel count exceeds bound");
        std::sort(next->regions.begin(),next->regions.end(),[](const auto& a,const auto& b){return a.region->id<b.region->id;});
        std::sort(next->channelsByPointer.begin(),next->channelsByPointer.end(),[](const auto& a,const auto& b){return a.source->channel<b.source->channel;});
        std::sort(next->channelsByIndex.begin(),next->channelsByIndex.end(),[](const auto& a,const auto& b){return a.source->channelIndex<b.source->channelIndex;});return next;
    }
    void replaceTable(std::unique_ptr<Table> next){
        demand(retired.size()<64,"standalone source old tables require an actual drained model boundary before more publication");
        retired.reserve(retired.size()+1);auto old=std::move(current);current=std::move(next);
        table.store(current.get(),std::memory_order_seq_cst);if(old)retired.push_back(std::move(old));collect();
    }
    void collect(){requireMain();if(readers.load(std::memory_order_seq_cst)==0)retired.clear();}
    void requireMatchingTicketsDisabled(const std::string& id)const{
        const auto check=[&](const Table* candidate){if(!candidate)return;for(const auto& batch:candidate->batches)
            if(batch->control->id==id)demand(!batch->cache.gate->authorizes(batch->cache.ticket),
                "standalone source table-only removal refuses an authorized matching ticket");};
        // Each immutable region/channel row in a batch has this exact shared
        // gate/ticket. Check current AND retired tables still visible to readers.
        check(current.get());for(const auto& old:retired)check(old.get());
        const auto found=live.find(id);if(found!=live.end())demand(!found->second->cache.gate->authorizes(found->second->cache.ticket),
            "standalone source table-only removal refuses its authorized live ticket");
    }
    static FLStandalonePhysicalIdentityCandidate diagnosticCandidate(const Batch* batch,const Source* source,const Region* region)noexcept{
        FLStandalonePhysicalIdentityCandidate value;if(!batch||!source)return value;
        value.present=true;value.batchAddress=reinterpret_cast<std::uintptr_t>(batch);value.sourceAddress=reinterpret_cast<std::uintptr_t>(source);
        value.actualNativeComponent=batch->control->component;value.captureSerial=batch->cache.captureSerial;value.ticketEpoch=batch->cache.ticket.epoch;
        value.publicationGeneration=batch->publicationGeneration;value.nativeMixerInsert=batch->mixer;value.expected=source->physicalExpected;
        if(region){value.regionPresent=true;value.nativeClipID=region->id;value.regionSourceIndex=region->sourceIndex;value.nativeRecord=region->record;}
        return value;
    }
    void capturePhysicalIdentityDiagnostic(const Table& table,const Batch& owner,const FLStandaloneSourceRead& read,
        FLStandalonePhysicalIdentityBranch branch,const Table::Entry* entry,const Table::Channel* physical,const Table::Channel* indexed,
        std::size_t matchingBatchCount)noexcept{
        // Called only under process()'s existing ReaderGuard, BEFORE fault()
        // invalidates anything. At most four strong CAS attempts, no wrapping
        // reservation counter, no retry loop, allocation, new read or blocking.
        auto& control=*owner.control;
        for(std::size_t i=0;i<control.physicalDiagnostics.size();++i){auto& slot=control.physicalDiagnostics[i];std::uint8_t empty=0;
            if(!slot.state.compare_exchange_strong(empty,1,std::memory_order_relaxed,std::memory_order_relaxed))continue;
            auto& value=slot.value;value.slotIndex=static_cast<std::uint8_t>(i);value.branch=branch;value.matchingBatchCount=static_cast<std::uint8_t>(matchingBatchCount);
            value.protectedTableAddress=reinterpret_cast<std::uintptr_t>(&table);value.ownerBatchAddress=reinterpret_cast<std::uintptr_t>(&owner);
            value.actualNativeComponent=control.component;value.tableBatchCount=table.batches.size();value.tableRegionCount=table.regions.size();
            value.tablePointerChannelCount=table.channelsByPointer.size();value.tableIndexedChannelCount=table.channelsByIndex.size();
            value.captureSerial=owner.cache.captureSerial;value.ticketEpoch=owner.cache.ticket.epoch;value.publicationGeneration=owner.publicationGeneration;value.observed=read;
            if(entry){value.presenceMask|=1;value.entry=diagnosticCandidate(entry->batch,entry->source,entry->region);if(entry->batch==&owner)value.agreementMask|=1;}
            if(physical){value.presenceMask|=2;value.physical=diagnosticCandidate(physical->batch,physical->source,nullptr);if(physical->batch==&owner)value.agreementMask|=2;}
            if(indexed){value.presenceMask|=4;value.indexed=diagnosticCandidate(indexed->batch,indexed->source,nullptr);if(indexed->batch==&owner)value.agreementMask|=4;}
            if(entry&&physical&&entry->source==physical->source)value.agreementMask|=8;
            if(entry&&indexed&&entry->source==indexed->source)value.agreementMask|=16;
            if(physical&&indexed&&physical->source==indexed->source)value.agreementMask|=32;
            const auto mismatch=[&](FLStandalonePhysicalIdentityMismatch bit){value.mismatchBits|=static_cast<std::uint32_t>(bit);};
            if(branch==FLStandalonePhysicalIdentityBranch::MappingAgreementA){
                if(!entry)mismatch(FLStandalonePhysicalIdentityMismatch::MissingEntry);
                if(!physical)mismatch(FLStandalonePhysicalIdentityMismatch::MissingPhysical);
                if(!indexed)mismatch(FLStandalonePhysicalIdentityMismatch::MissingIndexed);
                if(entry&&entry->batch!=&owner)mismatch(FLStandalonePhysicalIdentityMismatch::EntryBatch);
                if(physical&&physical->batch!=&owner)mismatch(FLStandalonePhysicalIdentityMismatch::PhysicalBatch);
                if(indexed&&indexed->batch!=&owner)mismatch(FLStandalonePhysicalIdentityMismatch::IndexedBatch);
                if(entry&&physical&&entry->source!=physical->source)mismatch(FLStandalonePhysicalIdentityMismatch::EntryPhysicalSource);
                if(entry&&indexed&&entry->source!=indexed->source)mismatch(FLStandalonePhysicalIdentityMismatch::EntryIndexedSource);
            }else if(branch==FLStandalonePhysicalIdentityBranch::ObservedPhysicalSourceB&&entry){const auto& source=*entry->source;
                if(!read.voiceObject)mismatch(FLStandalonePhysicalIdentityMismatch::MissingVoice);
                if(!read.readerObject)mismatch(FLStandalonePhysicalIdentityMismatch::MissingReader);
                if(read.channelObject!=source.channel)mismatch(FLStandalonePhysicalIdentityMismatch::ChannelObject);
                if(read.sampleObject!=source.sample)mismatch(FLStandalonePhysicalIdentityMismatch::SampleObject);
                if(read.nativeMixerInsert!=owner.mixer)mismatch(FLStandalonePhysicalIdentityMismatch::MixerInsert);
            }
            slot.state.store(2,std::memory_order_release);return;
        }
        control.physicalDiagnosticOverflow.store(true,std::memory_order_release);
    }
    template<class Row>static void copyPhysicalIdentityDiagnostics(const Control& control,Row& row)noexcept{
        // Acquire only Ready immutable slots. A Writing slot is skipped, never
        // waited for. No consumer clears readiness or re-arms a producer slot.
        for(std::size_t i=0;i<control.physicalDiagnostics.size();++i){const auto& slot=control.physicalDiagnostics[i];
            if(slot.state.load(std::memory_order_acquire)==2){row.physicalIdentityDiagnostics[i]=slot.value;row.physicalIdentityDiagnosticReadyMask|=static_cast<std::uint8_t>(1u<<i);}}
        row.physicalIdentityDiagnosticOverflow=control.physicalDiagnosticOverflow.load(std::memory_order_acquire);
    }
    void fault(const Table& t,const std::array<const Batch*,3>& matches,std::size_t count,FLStandaloneOverlayFault reason)noexcept{
        const auto bit=std::uint64_t(1)<<static_cast<unsigned>(reason);totals.rejected.fetch_add(1);totals.bits.fetch_or(bit);
        for(std::size_t i=0;i<count;++i){auto* b=matches[i];if(!b)continue;
            // Stop each matching instance immediately. No cross-instance global
            // disable and no unrelated sample-object/source-only association.
            b->cache.gate->invalidate();auto& c=*b->control;c.counters.rejected.fetch_add(1);c.counters.bits.fetch_or(bit);
            c.pendingBits.fetch_or(bit);c.faultCaptureSerial.store(b->cache.captureSerial);std::uint8_t zero=0;
            c.firstFault.compare_exchange_strong(zero,static_cast<std::uint8_t>(reason));}
        (void)t;config.ownerFaultQueued->store(true,std::memory_order_release);
    }
    FLStandaloneOverlayOutcome holdForCoherentRebuild(const Batch& batch,const FLStandaloneSourceRead& read)noexcept{
        // Called only AFTER the same complete physical/record/shape/range
        // validation used for a cache replacement. No guessed source is muted.
        auto& control=*batch.control;
        totals.stale.fetch_add(1);control.counters.stale.fetch_add(1);
        totals.held.fetch_add(1);control.counters.held.fetch_add(1);
        if(read.originalReadFrames>0){
            for(std::int32_t i=0;i<read.originalReadFrames;++i){read.nativeInterleavedStereo[i*2]=0;read.nativeInterleavedStereo[i*2+1]=0;}
            const auto frames=static_cast<std::uint64_t>(read.originalReadFrames);
            totals.heldFrames.fetch_add(frames);control.counters.heldFrames.fetch_add(frames);
        }
        // A retired in-flight reader may still hold its own stale span, but
        // cannot re-arm STOP/status for a newer coherent current publication.
        auto episode=control.holdEpisodeGeneration.load();
        while(episode!=batch.publicationGeneration){
            if(control.activePublicationGeneration.load()!=batch.publicationGeneration)break;
            if(control.holdEpisodeGeneration.compare_exchange_weak(episode,batch.publicationGeneration)){
                if(control.activePublicationGeneration.load()==batch.publicationGeneration){
                    control.holdDelivery.store(true,std::memory_order_release);
                    totals.holdRequests.fetch_add(1);control.counters.holdRequests.fetch_add(1);
                    config.ownerStaleHoldQueued->store(true,std::memory_order_release);
                }
                break;
            }
        }
        return FLStandaloneOverlayOutcome::HeldForRebuild;
    }
    void noteQualifiedPreviewUnrelated()noexcept{
        // Global observation only. Four bounded CAS attempts, saturate rather
        // than wrap. Contention can under-count; never authorizes a source/read.
        auto value=qualifiedPreviewUnrelatedCallbacks.load(std::memory_order_relaxed);
        for(unsigned attempt=0;attempt<4;++attempt){if(value==std::numeric_limits<std::uint64_t>::max())return;
            if(qualifiedPreviewUnrelatedCallbacks.compare_exchange_strong(value,value+1,std::memory_order_relaxed,std::memory_order_relaxed))return;}
    }
    FLStandaloneOverlayOutcome process(const FLStandaloneSourceRead& r)noexcept{
        ReaderGuard guard(readers);const auto* t=table.load(std::memory_order_seq_cst);if(!t)return FLStandaloneOverlayOutcome::Inactive;
        totals.calls.fetch_add(1);
        const Table::Entry* entry=nullptr;const Table::Channel* physical=nullptr;const Table::Channel* indexed=nullptr;
        if(r.channelObject){auto ch=std::lower_bound(t->channelsByPointer.begin(),t->channelsByPointer.end(),r.channelObject,[](const auto& row,std::uintptr_t pointer){return row.source->channel<pointer;});
            if(ch!=t->channelsByPointer.end()&&ch->source->channel==r.channelObject)physical=&*ch;}
        // Exact non-Playlist role proof precedes borrowed clip/index aliases.
        // A pointer present in ANY protected captured source is never exempted,
        // even if its current class/global ownership appears replaced/recycled.
        const bool independentlyOwnedPreview=!physical&&r.nativeObservationComplete&&r.voiceRecordRead&&r.voiceObject&&r.readerObject&&r.channelObject&&
            r.voiceVMT==config.actualEngineImageBase+voiceVMTOffset&&r.channelVMT==config.actualEngineImageBase+previewChannelVMTOffset&&
            r.globalPreviewOwnerReadCoherent&&r.globalPreviewChannelObject==r.channelObject;
        if(independentlyOwnedPreview){totals.unrelated.fetch_add(1);noteQualifiedPreviewUnrelated();return FLStandaloneOverlayOutcome::Unrelated;}
        if(r.voiceRecordRead){auto it=std::lower_bound(t->regions.begin(),t->regions.end(),r.nativeClipID,[](const auto& row,std::int32_t id){return row.region->id<id;});
            if(it!=t->regions.end()&&it->region->id==r.nativeClipID)entry=&*it;
            auto ch=std::lower_bound(t->channelsByIndex.begin(),t->channelsByIndex.end(),r.nativeChannelIndex,[](const auto& row,std::uint16_t index){return row.source->channelIndex<index;});
            if(ch!=t->channelsByIndex.end()&&ch->source->channelIndex==r.nativeChannelIndex)indexed=&*ch;}
        std::array<const Batch*,3> matches{};std::size_t count=0;
        const auto add=[&](const Batch* b){if(b&&std::find(matches.begin(),matches.begin()+count,b)==matches.begin()+count)matches[count++]=b;};
        add(entry?entry->batch:nullptr);add(physical?physical->batch:nullptr);add(indexed?indexed->batch:nullptr);
        if(!count){totals.unrelated.fetch_add(1);return FLStandaloneOverlayOutcome::Unrelated;}
        for(std::size_t i=0;i<count;++i)matches[i]->control->counters.calls.fetch_add(1);
        // A stale cache cannot dry-forward a fully proven affected source.
        // Identity/shape validation still precedes any silence hold or write.
        const auto deny=[&](FLStandaloneOverlayFault reason){fault(*t,matches,count,reason);return FLStandaloneOverlayOutcome::Rejected;};
        if(count!=1)return deny(FLStandaloneOverlayFault::AmbiguousTarget);
        const auto* b=matches[0];
        if(!entry||!physical||!indexed||entry->batch!=b||physical->batch!=b||indexed->batch!=b||entry->source!=physical->source||entry->source!=indexed->source){
            capturePhysicalIdentityDiagnostic(*t,*b,r,FLStandalonePhysicalIdentityBranch::MappingAgreementA,entry,physical,indexed,count);
            return deny(FLStandaloneOverlayFault::PhysicalIdentity);}
        const auto& s=*entry->source;
        if(r.voiceVMT&&r.voiceVMT!=config.actualEngineImageBase+voiceVMTOffset)
            return deny(FLStandaloneOverlayFault::ClassSeal);
        if(!r.nativeObservationComplete)return deny(FLStandaloneOverlayFault::NativeRead);
        if(r.voiceVMT!=config.actualEngineImageBase+voiceVMTOffset||r.channelVMT!=s.channelVMT||r.sampleVMT!=s.sampleVMT||r.readerVMT!=config.actualEngineImageBase+readerVMTOffset)
            return deny(FLStandaloneOverlayFault::ClassSeal);
        if(r.nativeRecord!=entry->region->record||r.nativeClipID!=entry->region->id)return deny(FLStandaloneOverlayFault::RecordIdentity);
        if(!r.voiceObject||!r.readerObject||r.channelObject!=s.channel||r.sampleObject!=s.sample||r.nativeMixerInsert!=b->mixer){
            capturePhysicalIdentityDiagnostic(*t,*b,r,FLStandalonePhysicalIdentityBranch::ObservedPhysicalSourceB,entry,physical,indexed,count);
            return deny(FLStandaloneOverlayFault::PhysicalIdentity);}
        if(r.channelSample328!=s.sample||r.readerSample78!=s.sample||r.nativePCM90!=s.pcm||r.readerPCM40!=s.pcm)
            return deny(FLStandaloneOverlayFault::PointerRelation);
        if(r.nativeSourceFrames!=s.frames||r.nativeRate!=48000||r.nativeRateFactor!=1||r.nativeFormat78!=s.channels||r.nativeFormat79!=0)
            return deny(FLStandaloneOverlayFault::NativeSourceMetadata);
        if(r.readerChannelsA0!=s.channels||r.readerFormatA1!=0||r.readerStride48!=std::int32_t(s.channels))
            return deny(FLStandaloneOverlayFault::NativeReaderMetadata);
        if(r.referenceC8||r.stretchProvider100||r.readerStep!=1)return deny(FLStandaloneOverlayFault::UnsupportedSourceTransform);
        if(!r.nativeInterleavedStereo||!r.outputMatchesActualNativeContext||r.nativeBlockFrames<1||r.nativeBlockFrames>maximumBlockFrames||
            r.originalReadFrames<0||r.originalReadFrames>r.nativeBlockFrames)return deny(FLStandaloneOverlayFault::OutputShape);
        if(!std::isfinite(r.sourceStartFrame)||r.sourceStartFrame<0||std::floor(r.sourceStartFrame)!=r.sourceStartFrame)
            return deny(FLStandaloneOverlayFault::SourcePosition);
        if(!r.originalReadFrames){if(r.sourceStartFrame>double(s.frames)&&r.originalEnded!=1)return deny(FLStandaloneOverlayFault::SourcePosition);
            if(!b->cache.gate->authorizes(b->cache.ticket)){
                totals.zero.fetch_add(1);b->control->counters.zero.fetch_add(1);return holdForCoherentRebuild(*b,r);}
            totals.zero.fetch_add(1);b->control->counters.zero.fetch_add(1);return FLStandaloneOverlayOutcome::NativeZeroRead;}
        if(r.sourceStartFrame>double(s.frames))return deny(FLStandaloneOverlayFault::SourcePosition);
        const auto start=std::uint64_t(r.sourceStartFrame),frames=std::uint64_t(r.originalReadFrames);
        if(frames>std::uint64_t(s.frames)-start)return deny(FLStandaloneOverlayFault::SourceRange);
        // Recheck immediately before the first write. Once a notification has
        // closed the gate, a later proven read holds instead of forwarding dry.
        if(!b->cache.gate->authorizes(b->cache.ticket))return holdForCoherentRebuild(*b,r);
        const auto* left=s.cache->channels[0].data()+start;const auto* right=s.cache->channels[s.channels==1?0:1].data()+start;
        for(std::uint64_t i=0;i<frames;++i){r.nativeInterleavedStereo[i*2]=left[i];r.nativeInterleavedStereo[i*2+1]=right[i];}
        // If invalidation was observed during this copy, replace the proven
        // span with a hold. Audio consumed before this check is not retractable.
        if(!b->cache.gate->authorizes(b->cache.ticket))return holdForCoherentRebuild(*b,r);
        totals.applied.fetch_add(1);totals.frames.fetch_add(frames);b->control->counters.applied.fetch_add(1);b->control->counters.frames.fetch_add(frames);
        return FLStandaloneOverlayOutcome::Applied;
    }
};

FLStandaloneSourceOverlay::FLStandaloneSourceOverlay(FLStandaloneOverlayConfig config):p(std::make_unique<Impl>(std::move(config))){}
FLStandaloneSourceOverlay::~FLStandaloneSourceOverlay(){
    // Root must explicitly restore the source DATA slot at its native boundary
    // before this module/code/owner mailbox can be destroyed.
    if(p->installed||p->readers.load(std::memory_order_seq_cst))std::terminate();
}
FLStandaloneOverlayInstallResult FLStandaloneSourceOverlay::installLoadedNativeSourceHook(){
    p->requireBoundary();demand(!p->installed&&!hook.router.load()&&!hook.original.load(),"standalone source hook already owned/installed");
    const auto base=p->config.actualEngineImageBase;const auto path=canonical(p->config.exactLoadedEnginePath);
    demand(!path.empty()&&path==p->config.exactLoadedEnginePath,"standalone source admitted engine path is not exact canonical path");
    Dl_info image{};demand(dladdr(reinterpret_cast<void*>(base+sourceReadOffset),&image)&&image.dli_fbase&&image.dli_fname&&
        reinterpret_cast<std::uintptr_t>(image.dli_fbase)==base&&canonical(image.dli_fname)==path,"standalone source callback is not in the admitted already-loaded engine");
    mach_header_64 header{};demand(boundedRead(base,&header,sizeof(header))&&header.magic==MH_MAGIC_64&&header.cputype==CPU_TYPE_ARM64&&
        stableArm64SliceSHA(path)==p->config.exactEngineArm64SliceSHA256,"standalone source loaded engine ARM64 slice/header differs from seal");
    const auto slot=base+voiceVMTOffset+sourceReadSlot;std::uintptr_t actual=0;
    demand(boundedRead(slot,&actual,sizeof(actual))&&actual==base+sourceReadOffset,"standalone source native VMT method is not the exact original callback");
    p->requireBoundary();hook.engineBase.store(base,std::memory_order_seq_cst);hook.original.store(reinterpret_cast<FLStandaloneNativeVoiceRead>(actual),std::memory_order_seq_cst);hook.router.store(this,std::memory_order_seq_cst);
    const auto result=replaceCOWDataPointer(slot,actual,reinterpret_cast<std::uintptr_t>(&FLStandaloneOverlayNativeThunk::read));
    if(!result.changed){hook.router.store(nullptr);hook.original.store(nullptr);hook.engineBase.store(0);throw std::runtime_error("standalone source private COW data-slot publication failed");}
    p->installed=true;p->installedSlot=slot;
    demand(result.protectionRestored,"standalone source COW data page protection restoration failed; root must stop integration at its boundary");
    return {true,slot,actual,"exact admitted native source callback installed through private COW data slot"};
}
void FLStandaloneSourceOverlay::publish(const std::string& id,const FLStandaloneDecodedGraph& decoded,
        std::shared_ptr<const FLNativeModelSnapshot> model,StandaloneSourceCacheBatch cache){
    p->requireBoundary();auto next=p->build(id,decoded,std::move(model),std::move(cache));const auto previous=p->live.find(id);
    if(previous!=p->live.end()&&previous->second->cache.gate==next->cache.gate)
        demand(next->cache.ticket.epoch>previous->second->cache.ticket.epoch,"standalone source stale/repeated cache ticket cannot replace a publication");
    auto instances=p->live;instances[id]=next;auto table=p->makeTable(instances);p->collect();
    demand(p->retired.size()<64,"standalone source retired tables have not drained");p->retired.reserve(p->retired.size()+1);
    p->requireBoundary();demand(next->cache.gate->authorizes(next->cache.ticket),"standalone source ticket changed during immutable table preparation");
    if(previous!=p->live.end()&&previous->second->cache.gate!=next->cache.gate)previous->second->cache.gate->invalidate();
    next->control->activePublicationGeneration=next->publicationGeneration;
    next->control->holdEpisodeGeneration=0;next->control->holdDelivery=false;
    p->controls[id]=next->control;p->live=std::move(instances);p->replaceTable(std::move(table));
}
void FLStandaloneSourceOverlay::invalidateInstance(const std::string& id)noexcept{
    ReaderGuard guard(p->readers);const auto* table=p->table.load(std::memory_order_seq_cst);if(!table)return;
    for(const auto& batch:table->batches)if(batch->control->id==id)batch->cache.gate->invalidate();
}
void FLStandaloneSourceOverlay::removeInstanceAtNativeBoundary(const std::string& id){
    p->requireBoundary();auto instances=p->live;const auto current=instances.find(id);if(current==instances.end())return;
    auto removed=current->second;instances.erase(current);auto table=p->makeTable(instances);p->collect();
    demand(p->retired.size()<64,"standalone source retired tables have not drained");p->retired.reserve(p->retired.size()+1);
    p->requireBoundary();removed->cache.gate->invalidate();p->live=std::move(instances);p->replaceTable(std::move(table));
}
void FLStandaloneSourceOverlay::removeDisabledInstanceOnModelThread(const std::string& id){
    p->requireAdmittedModelMaintenance();p->requireMatchingTicketsDisabled(id);
    auto instances=p->live;const auto found=instances.find(id);
    if(found==instances.end()){p->collect();return;}
    instances.erase(found);auto table=p->makeTable(instances);p->collect();
    demand(p->retired.size()<64,"standalone source disabled table retirement has not drained");p->retired.reserve(p->retired.size()+1);
    p->requireAdmittedModelMaintenance();p->requireMatchingTicketsDisabled(id);
    // No gate mutation, native-state/engine read, ARA/vendor call or fabricated
    // stopped capture. Old readers retain immutable source floats independently
    // of any document controller/native component already being torn down.
    p->live=std::move(instances);p->replaceTable(std::move(table));
}
void FLStandaloneSourceOverlay::collectRetiredOnModelThread(){p->collect();}
FLStandaloneOverlayOutcome FLStandaloneSourceOverlay::processObservedAfterOriginal(const FLStandaloneSourceRead& read)noexcept{return p->process(read);}
FLStandaloneOverlayCounters FLStandaloneSourceOverlay::counters()const noexcept{auto result=p->totals.snapshot(p->readers.load(std::memory_order_seq_cst));
    result.qualifiedPreviewUnrelatedCallbacks=p->qualifiedPreviewUnrelatedCallbacks.load(std::memory_order_relaxed);return result;}
std::vector<FLStandaloneOverlayInstanceStats> FLStandaloneSourceOverlay::instanceStatsOnModelThread()const{
    p->requireMain();std::vector<FLStandaloneOverlayInstanceStats> rows;rows.reserve(p->controls.size());
    for(const auto& pair:p->controls){FLStandaloneOverlayInstanceStats row;row.instanceID=pair.first;row.counters=pair.second->counters.snapshot();
        Impl::copyPhysicalIdentityDiagnostics(*pair.second,row);
        const auto found=p->live.find(pair.first);if(found!=p->live.end()){const auto& batch=*found->second;row.nativeMixerInsert=batch.mixer;row.captureSerial=batch.cache.captureSerial;
            row.ticketEpoch=batch.cache.ticket.epoch;row.sources=batch.sources.size();row.regions=batch.regions.size();row.published=true;row.ticketAuthorized=batch.cache.gate->authorizes(batch.cache.ticket);
            row.staleHoldActive=batch.control->holdEpisodeGeneration.load()==batch.publicationGeneration;}rows.push_back(std::move(row));}return rows;
}
std::vector<FLStandaloneOverlayFaultRecord> FLStandaloneSourceOverlay::takeFaultsOnModelThread(){
    p->requireMain();std::vector<FLStandaloneOverlayFaultRecord> out;
    for(const auto& pair:p->controls){auto& c=*pair.second;const auto bits=c.pendingBits.exchange(0,std::memory_order_acq_rel);if(!bits)continue;
        auto code=c.firstFault.exchange(0);if(code==0||!(bits&(std::uint64_t(1)<<code))){code=1;while(code<63&&!(bits&(std::uint64_t(1)<<code)))++code;}
        // Bits are the authoritative coalesced fault set. A notification racing
        // this drain can reset the first-code hint; retain a valid reason from
        // the exact drained bits rather than reporting None for a real fault.
        FLStandaloneOverlayFaultRecord row{pair.first,c.component,c.faultCaptureSerial.load(),bits,static_cast<FLStandaloneOverlayFault>(code)};
        Impl::copyPhysicalIdentityDiagnostics(c,row);out.push_back(std::move(row));}return out;
}
std::vector<FLStandaloneOverlayStaleHoldRecord> FLStandaloneSourceOverlay::takeStaleHoldsOnModelThread(){
    p->requireMain();std::vector<FLStandaloneOverlayStaleHoldRecord> out;
    for(const auto& pair:p->controls){auto& c=*pair.second;if(!c.holdDelivery.exchange(false,std::memory_order_acq_rel))continue;
        const auto current=p->live.find(pair.first);if(current==p->live.end())continue;const auto& batch=*current->second;
        if(c.holdEpisodeGeneration.load()==batch.publicationGeneration&&!batch.cache.gate->authorizes(batch.cache.ticket))
            out.push_back({pair.first,c.component,batch.cache.captureSerial,batch.cache.ticket.epoch});}
    return out;
}
void FLStandaloneSourceOverlay::shutdownAtNativeBoundary(){
    p->requireBoundary();for(const auto& pair:p->live)pair.second->cache.gate->invalidate();
    if(p->installed){demand(hook.router.load()==this&&hook.original.load(),"standalone source hook ownership changed before shutdown");
        const auto original=reinterpret_cast<std::uintptr_t>(hook.original.load());
        const auto restored=replaceCOWDataPointer(p->installedSlot,reinterpret_cast<std::uintptr_t>(&FLStandaloneOverlayNativeThunk::read),original);
        demand(restored.changed&&restored.protectionRestored,"standalone source own COW data slot could not be restored");
        hook.router.store(nullptr,std::memory_order_seq_cst);demand(!hook.readers.load(std::memory_order_seq_cst),"standalone source callback did not drain at native shutdown");
        hook.original.store(nullptr,std::memory_order_seq_cst);hook.engineBase.store(0,std::memory_order_seq_cst);p->installed=false;}
    auto old=std::move(p->current);p->table.store(nullptr,std::memory_order_seq_cst);if(old)p->retired.push_back(std::move(old));
    p->live.clear();p->collect();demand(p->retired.empty(),"standalone source retired tables are not drained");p->shutDown=true;
}
void FLStandaloneSourceOverlay::processNativeAfterOriginal(void* self,void* context,float** output,
        std::int32_t* frames,std::uint8_t* ended,std::uint8_t* changed,double pre,bool preRead)noexcept{
    // No target table means original-only forwarding, with no native body reads.
    if(!p->table.load(std::memory_order_seq_cst))return;
    FLStandaloneSourceRead read;read.voiceObject=reinterpret_cast<std::uintptr_t>(self);read.preOriginalReaderPosition=pre;read.preOriginalReaderPositionRead=preRead;
    const auto base=p->config.actualEngineImageBase;std::array<std::uint8_t,0x3e0> voice{};
    std::uintptr_t previewOwnerBefore=0;
    const bool previewOwnerBeforeRead=readAt(base,globalPreviewChannelOffset,previewOwnerBefore);
    const bool vmtRead=readAt(read.voiceObject,0,read.voiceVMT);
    const bool voiceRead=vmtRead&&read.voiceVMT==base+voiceVMTOffset&&boundedRead(read.voiceObject,voice.data(),voice.size());
    if(voiceRead){read.voiceVMT=fromBytes<std::uintptr_t>(voice.data(),0);read.channelObject=fromBytes<std::uintptr_t>(voice.data(),0x270);
        read.sampleObject=fromBytes<std::uintptr_t>(voice.data(),0x10);read.readerObject=fromBytes<std::uintptr_t>(voice.data(),0x300);
        std::memcpy(read.nativeRecord.data(),voice.data()+0x358,88);read.nativeClipID=fromBytes<std::int32_t>(read.nativeRecord.data(),0x20);
        read.nativeChannelIndex=fromBytes<std::uint16_t>(read.nativeRecord.data(),6);read.sourceStartFrame=fromBytes<double>(voice.data(),0x3d8);read.voiceRecordRead=true;}
    else{
        // If the full sealed body was unreadable, recover only independent
        // bounded identity fields. A recognizable target still closes its gate;
        // an unrelated/unknown voice does not disable every edited insert.
        readAt(read.voiceObject,0x270,read.channelObject);
        read.voiceRecordRead=readAt(read.voiceObject,0x358,read.nativeRecord);
        if(read.voiceRecordRead){read.nativeClipID=fromBytes<std::int32_t>(read.nativeRecord.data(),0x20);
            read.nativeChannelIndex=fromBytes<std::uint16_t>(read.nativeRecord.data(),6);}
    }
    std::array<std::uint8_t,0x330> channel{};
    std::array<std::uint8_t,0x1b0> sample{};std::array<std::uint8_t,0x108> reader{};
    const bool complete=voiceRead&&read.voiceVMT==base+voiceVMTOffset&&boundedRead(read.channelObject,channel.data(),channel.size())&&
        boundedRead(read.sampleObject,sample.data(),sample.size())&&boundedRead(read.readerObject,reader.data(),reader.size());
    std::uintptr_t actualOutput=0;
    if(complete){read.channelVMT=fromBytes<std::uintptr_t>(channel.data(),0);read.nativeMixerInsert=fromBytes<std::int32_t>(channel.data(),0x248);
        read.channelSample328=fromBytes<std::uintptr_t>(channel.data(),0x328);read.sampleVMT=fromBytes<std::uintptr_t>(sample.data(),0);
        read.nativePCM90=fromBytes<std::uintptr_t>(sample.data(),0x90);read.referenceC8=fromBytes<std::uintptr_t>(sample.data(),0xc8);
        read.nativeSourceFrames=fromBytes<std::int64_t>(sample.data(),0x98);read.nativeRate=fromBytes<double>(sample.data(),0x88);read.nativeRateFactor=fromBytes<double>(sample.data(),0x1a0);
        read.nativeFormat78=sample[0x78];read.nativeFormat79=sample[0x79];read.readerVMT=fromBytes<std::uintptr_t>(reader.data(),0);
        read.readerSample78=fromBytes<std::uintptr_t>(reader.data(),0x78);read.readerPCM40=fromBytes<std::uintptr_t>(reader.data(),0x40);
        read.stretchProvider100=fromBytes<std::uintptr_t>(reader.data(),0x100);read.readerStep=fromBytes<double>(reader.data(),0x10);
        read.readerChannelsA0=reader[0xa0];read.readerFormatA1=reader[0xa1];read.readerStride48=fromBytes<std::int32_t>(reader.data(),0x48);
        read.nativeObservationComplete=readAt(base,blockFramesOffset,read.nativeBlockFrames)&&
            readAt(reinterpret_cast<std::uintptr_t>(context),0x68,actualOutput)&&
            boundedRead(reinterpret_cast<std::uintptr_t>(output),&read.nativeInterleavedStereo,sizeof(read.nativeInterleavedStereo))&&
            boundedRead(reinterpret_cast<std::uintptr_t>(frames),&read.originalReadFrames,sizeof(read.originalReadFrames))&&
            boundedRead(reinterpret_cast<std::uintptr_t>(ended),&read.originalEnded,sizeof(read.originalEnded))&&
            boundedRead(reinterpret_cast<std::uintptr_t>(changed),&read.originalChanged,sizeof(read.originalChanged));
        read.outputMatchesActualNativeContext=actualOutput==reinterpret_cast<std::uintptr_t>(read.nativeInterleavedStereo);}
    std::uintptr_t previewOwnerAfter=0;
    const bool previewOwnerAfterRead=readAt(base,globalPreviewChannelOffset,previewOwnerAfter);
    recordDefaultPreviewOwnerObservation(read,previewOwnerBeforeRead,previewOwnerBefore,previewOwnerAfterRead,previewOwnerAfter);
    processObservedAfterOriginal(read);
}
} // namespace rysup::ara_lab

#ifdef FL_STANDALONE_SOURCE_OVERLAY_OFFLINE_TEST_MAIN
// Synthetic metadata/table controls ONLY. No engine/plug-in is loaded, no
// vtable is installed and no actual-native capture token is manufactured.
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>
namespace {
thread_local bool overlayOfflineRT=false;
std::atomic<std::uint64_t> overlayOfflineRTAllocations{0};
}
void* operator new(std::size_t count){if(overlayOfflineRT)overlayOfflineRTAllocations.fetch_add(1);if(auto* p=std::malloc(count?count:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t count){return ::operator new(count);}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}
void operator delete[](void* value,std::size_t)noexcept{std::free(value);}

namespace rysup::ara_lab {
struct FLStandaloneSourceOverlayOfflineAccess{
    static std::shared_ptr<const void> retainFirstBatch(FLStandaloneSourceOverlay& router){return router.p->current->batches.front();}
    static FLStandaloneOverlayOutcome holdRetired(FLStandaloneSourceOverlay& router,const std::shared_ptr<const void>& retained,const FLStandaloneSourceRead& read)noexcept{
        return router.p->holdForCoherentRebuild(*static_cast<const FLStandaloneSourceOverlay::Impl::Batch*>(retained.get()),read);
    }
    static const void* borrow(FLStandaloneSourceOverlay& router)noexcept{
        router.p->readers.fetch_add(1,std::memory_order_seq_cst);
        return router.p->table.load(std::memory_order_seq_cst);
    }
    static void release(FLStandaloneSourceOverlay& router)noexcept{router.p->readers.fetch_sub(1,std::memory_order_seq_cst);}
    static std::size_t regionCount(const void* pointer)noexcept{return static_cast<const FLStandaloneSourceOverlay::Impl::Table*>(pointer)->regions.size();}
    static float cacheValue(const void* pointer)noexcept{return static_cast<const FLStandaloneSourceOverlay::Impl::Table*>(pointer)->regions[0].source->cache->channels[0][2];}
    static std::size_t retiredCount(const FLStandaloneSourceOverlay& router)noexcept{return router.p->retired.size();}
    static void seedQualifiedPreviewCounter(FLStandaloneSourceOverlay& router,std::uint64_t value)noexcept{router.p->qualifiedPreviewUnrelatedCallbacks.store(value);}
    static void seedUnrelatedLegacyCountersNearWrap(FLStandaloneSourceOverlay& router)noexcept{
        for(auto& item:router.p->controls){item.second->counters.calls.store(std::numeric_limits<std::uint64_t>::max()-2);
            item.second->counters.rejected.store(std::numeric_limits<std::uint64_t>::max()-2);}
    }
    static void diagnoseBorrowedTable(FLStandaloneSourceOverlay& router,const void* raw,const FLStandaloneSourceRead& read)noexcept{
        const auto& table=*static_cast<const FLStandaloneSourceOverlay::Impl::Table*>(raw);const auto& entry=table.regions[0];
        router.p->capturePhysicalIdentityDiagnostic(table,*entry.batch,read,FLStandalonePhysicalIdentityBranch::ObservedPhysicalSourceB,
            &entry,&table.channelsByPointer[0],&table.channelsByIndex[0],1);
    }
};
namespace {
constexpr std::uintptr_t syntheticBase=0x100000000ULL;
constexpr float marker=.625f;
struct SyntheticOwner{std::atomic<bool> admitted{true},held{true},fault{false},staleHold{false};std::atomic<unsigned> holdChecks{0};};
bool syntheticAdmission(void* raw)noexcept{return static_cast<SyntheticOwner*>(raw)->admitted.load();}
bool syntheticHold(void* raw)noexcept{auto& owner=*static_cast<SyntheticOwner*>(raw);owner.holdChecks.fetch_add(1);return owner.held.load();}
FLStandaloneOverlayConfig syntheticConfig(SyntheticOwner& owner){return {syntheticBase,"/SYNTHETIC/no-native-image-loaded",
    kFLStandaloneOverlayEngineArm64SHA256,syntheticAdmission,syntheticHold,&owner,&owner.fault,&owner.staleHold};}
template<class T>void putSynthetic(Record& bytes,std::size_t offset,T value){std::memcpy(bytes.data()+offset,&value,sizeof(value));}
void updateSynthetic(FLNativeClipSnapshot& clip){
    putSynthetic(clip.record,0,clip.timelineStartTicks00);putSynthetic(clip.record,4,clip.packedTypeAndChannel04);
    putSynthetic(clip.record,8,clip.timelineLengthTicks08);putSynthetic(clip.record,12,clip.nativeTrack0C);
    putSynthetic(clip.record,24,clip.nativeSourceStart18);putSynthetic(clip.record,28,clip.nativeSourceEnd1C);putSynthetic(clip.record,32,clip.nativeClipID20);
}
std::shared_ptr<FLNativeModelSnapshot> syntheticModel(){
    auto model=std::make_shared<FLNativeModelSnapshot>();model->engineImageBase=syntheticBase;model->loadedImagePath="/SYNTHETIC/no-native-image-loaded";
    model->verifiedArm64SliceSHA256=kFLStandaloneOverlayEngineArm64SHA256;model->repeatedReadsMatched=true;
    model->nativeRecordStride=88;model->nativeChannelManager=0x2a0000;model->nativeChannelPointerArray=0x2b0000;model->nativeChannelCount=5;model->currentArrangementIndex=0;model->currentArrangementObject=0x200000;
    model->arrangementObjects={model->currentArrangementObject};
    for(int c=0;c<2;++c){FLNativeSourceSnapshot source;source.waveSampleObject=0x300000+c*0x1000;
        source.actualWaveSampleVMT=syntheticBase+sampleVMTOffset;source.nativePCMAddress90=0x500000+c*0x1000;
        source.nativeFrameCount98=96000;source.nativeRate88=48000;source.nativeRateFactor1A0=1;
        source.qualifiedEncoding=FLNativePCMEncoding::SignedInt16LE;source.qualifiedChannelCount=c+1;
        source.nativeFormatByte78=c+1;source.nativeFormatByte79=0;source.pcmQualificationEvidenceID="SYNTHETIC OFFLINE source metadata, never a native-runtime claim";
        source.pcmBytes.reserve(96000*(c+1)*2);
        for(int frame=0;frame<96000;++frame)for(int ch=0;ch<=c;++ch){const auto value=std::int16_t((frame*17+ch*13)%65536-32768);
            source.pcmBytes.push_back(std::uint8_t(value));source.pcmBytes.push_back(std::uint8_t(std::uint16_t(value)>>8));}
        model->sources.push_back(std::move(source));FLNativeChannelSnapshot channel;channel.managerIndex=3+c;channel.channelObject=0x600000+c*0x1000;
        channel.actualChannelVMT=syntheticBase+channelVMTOffset;channel.nativeChannelType150=4;channel.nativeMixerInsert248=1+c;
        channel.nativeMixerInsertSupported=true;channel.sourceSnapshotIndex=c;channel.nativeWaveSampleObject328=model->sources[c].waveSampleObject;
        channel.actualNativeWaveSampleVMT=model->sources[c].actualWaveSampleVMT;model->channels.push_back(channel);
        for(int r=0;r<(c==0?2:1);++r){FLNativeClipSnapshot clip;clip.nativeListIndex=model->clips.size();clip.nativeClipID20=int(model->clips.size())+1;
            clip.channelSnapshotIndex=c;clip.channelIndex06=channel.managerIndex;clip.packedTypeAndChannel04=0x5000|(std::uint32_t(channel.managerIndex)<<16);
            clip.kind=FLNativeRecordKind::AudioChannel;clip.nativeTrack0C=c;clip.timelineStartTicks00=r?5760:0;clip.timelineLengthTicks08=r?1920:3840;
            clip.nativeSourceStart18=r?250.0f:0.0f;clip.nativeSourceEnd1C=r?1250.0f:2000.0f;updateSynthetic(clip);model->clips.push_back(clip);}}
    model->allNativeManagerChannelsInspected=true;
    for(int i=0;i<model->nativeChannelCount;++i){FLNativeManagerMemberSnapshot member;
        member.managerIndex=i;member.actualChannelObject=0xb00000+i*0x1000;member.actualChannelVMT=syntheticBase+channelVMTOffset;member.nativeChannelType150=0;
        for(const auto& channel:model->channels)if(channel.managerIndex==i)member={channel.managerIndex,channel.channelObject,channel.actualChannelVMT,channel.nativeChannelType150};
        model->nativeManagerMembers.push_back(member);}
    model->nativeRecordCount=int(model->clips.size());return model;
}
FLStandaloneDecodedGraph syntheticDecoded(const FLNativeModelSnapshot& model,int mixer){
    FLStandaloneDecodedGraph d;d.actualCurrentNativeRAMCopiedAndVerified=true; // Contract input only, explicitly synthetic fixture.
    d.noNativeSerializationOrFileAudioFallbackUsed=true;d.actualAssociation={std::uintptr_t(0x700000+mixer),std::uintptr_t(0x800000+mixer),mixer,"SYNTHETIC offline pointer-association fixture",true};
    d.nativeArrangementIndex=0;d.clock.engineImageBase=syntheticBase;d.clock.engineArm64SliceSHA256=kFLStandaloneOverlayEngineArm64SHA256;
    d.clock.ppq=960;d.clock.tempoMilliBPM=120000;d.clock.bpm=120;d.clock.actualDirectNativeReadTwiceAndEqual=true;
    std::memcpy(d.clock.actualClockBytes.data(),&d.clock.ppq,2);std::memcpy(d.clock.actualClockBytes.data()+2,&d.clock.tempoMilliBPM,4);
    for(const auto& clip:model.clips)d.actualCopiedNativeModelRecords.insert(d.actualCopiedNativeModelRecords.end(),clip.record.begin(),clip.record.end());
    d.nativeRecordModelSHA256=sha(d.actualCopiedNativeModelRecords.data(),d.actualCopiedNativeModelRecords.size());
    d.sourcePCMModelSHA256=std::string(64,'b');d.graphSHA256=std::string(64,'c');
    for(const auto& channel:model.channels)if(channel.nativeMixerInsert248==mixer){const auto& sample=model.sources[channel.sourceSnapshotIndex];
        const auto digest=sha(sample.pcmBytes.data(),sample.pcmBytes.size()),key="fl:native-channel:"+std::to_string(channel.managerIndex);
        const auto id=key+":pcm:"+digest,mod=id+":modification";
        d.sources.push_back({id,key,digest,channel.managerIndex,mixer,sample.nativeFrameCount98,sample.qualifiedChannelCount,48000,1,sample.nativeFormatByte78,0,sample.pcmQualificationEvidenceID});
        d.modifications.push_back({mod,key,id});d.graph.sources.push_back({id,"",sample.nativeFrameCount98,48000,int(sample.qualifiedChannelCount)});
        d.graph.modifications.push_back({mod,id,""});
        for(const auto& clip:model.clips)if(clip.channelIndex06==channel.managerIndex){const auto physical="fl:native-clip:"+std::to_string(clip.nativeClipID20),region=physical+":"+id+":region";
            d.regions.push_back({region,physical,mod,id,clip.nativeClipID20,channel.managerIndex,clip.nativeListIndex,clip.record,"",""});
            d.graph.regions.push_back({region,mod,"synthetic-track","",clip.nativeSourceStart18*.001,(clip.nativeSourceEnd1C-clip.nativeSourceStart18)*.001,0,1,ARA::kARAPlaybackTransformationNoChanges});}}
    return d;
}
StandaloneSourceCacheBatch syntheticBatch(const std::string& id,const FLStandaloneDecodedGraph& decoded,
        const std::shared_ptr<StandaloneCacheGate>& gate,std::uint64_t serial=1,float sign=1){
    StandaloneSourceCacheBatch b;b.instanceID=id;b.gate=gate;b.ticket=gate->invalidate();b.captureSerial=serial;b.actualEditNotificationRevision=5;
    b.native.nativeEngineSHA256=kFLStandaloneOverlayEngineArm64SHA256;b.native.nativeRecordModelSHA256=decoded.nativeRecordModelSHA256;
    b.native.sourcePCMModelSHA256=decoded.sourcePCMModelSHA256;b.native.graphSHA256=decoded.graphSHA256;b.native.nativeSaveVersion=decoded.nativeSaveVersion;
    b.native.clock.sampleRate=48000;b.native.clock.ppq=decoded.clock.ppq;b.native.clock.tempoMilliBPM=decoded.clock.tempoMilliBPM;
    b.native.clock.actualNativeClockBytes.assign(decoded.clock.actualClockBytes.begin(),decoded.clock.actualClockBytes.end());
    for(const auto& s:decoded.graph.sources){WholeModificationSourceCache cache;cache.source=s;cache.modificationID=s.id+":modification";
        cache.generation=serial;cache.editNotificationSerial=5;cache.actualDocumentController=0x900001;
        cache.actualAudioSource=0x900100+std::uintptr_t(decoded.actualAssociation.mixerInsert);cache.actualAudioModification=0x900200+std::uintptr_t(decoded.actualAssociation.mixerInsert);
        cache.actualOwnedPlaybackComponent=0x900300+std::uintptr_t(decoded.actualAssociation.mixerInsert);cache.internalRegionRemoved=true;cache.offline=true;
        cache.renderedChannels=s.channelCount;cache.channels.resize(s.channelCount);
        for(int ch=0;ch<s.channelCount;++ch){cache.channels[ch].reserve(std::size_t(s.sampleCount));for(std::int64_t f=0;f<s.sampleCount;++f)
            cache.channels[ch].push_back(sign*float((f%1003)+1)/1024.0f*(ch?-1.0f:1.0f));}b.modifications.push_back(std::move(cache));}
    demand(gate->publishIfUnchanged(b.ticket),"synthetic gate setup failed");return b;
}
FLStandaloneSourceRead syntheticRead(const FLNativeModelSnapshot& model,int clipID,float* output){
    const auto found=std::find_if(model.clips.begin(),model.clips.end(),[&](const auto& c){return c.nativeClipID20==clipID;});demand(found!=model.clips.end(),"synthetic clip absent");
    const auto& channel=model.channels[found->channelSnapshotIndex];const auto& sample=model.sources[channel.sourceSnapshotIndex];
    FLStandaloneSourceRead r;r.voiceRecordRead=true;r.nativeObservationComplete=true;r.voiceObject=0xa00000;r.voiceVMT=syntheticBase+voiceVMTOffset;
    r.channelObject=channel.channelObject;r.channelVMT=channel.actualChannelVMT;r.sampleObject=sample.waveSampleObject;r.sampleVMT=sample.actualWaveSampleVMT;
    r.readerObject=0xa10000;r.readerVMT=syntheticBase+readerVMTOffset;r.channelSample328=sample.waveSampleObject;r.readerSample78=sample.waveSampleObject;
    r.nativePCM90=sample.nativePCMAddress90;r.readerPCM40=sample.nativePCMAddress90;r.nativeClipID=clipID;r.nativeMixerInsert=channel.nativeMixerInsert248;
    r.nativeChannelIndex=channel.managerIndex;r.nativeRecord=found->record;r.nativeSourceFrames=sample.nativeFrameCount98;r.nativeRate=48000;r.nativeRateFactor=1;r.readerStep=1;
    r.sourceStartFrame=clipID==2?12000:2;r.nativeFormat78=sample.nativeFormatByte78;r.nativeFormat79=0;r.readerChannelsA0=sample.nativeFormatByte78;r.readerFormatA1=0;
    r.readerStride48=sample.nativeFormatByte78;r.nativeBlockFrames=6;r.originalReadFrames=3;r.nativeInterleavedStereo=output;r.outputMatchesActualNativeContext=true;return r;
}
struct OfflineFixture{
    SyntheticOwner owner;FLStandaloneSourceOverlay router{syntheticConfig(owner)};
    std::shared_ptr<FLNativeModelSnapshot> model=syntheticModel();
    FLStandaloneDecodedGraph mono=syntheticDecoded(*model,1),stereo=syntheticDecoded(*model,2);
    std::shared_ptr<StandaloneCacheGate> monoGate=std::make_shared<StandaloneCacheGate>(),stereoGate=std::make_shared<StandaloneCacheGate>();
    std::array<float,14> output{};
    OfflineFixture(){output.fill(marker);router.publish("mono",mono,model,syntheticBatch("mono",mono,monoGate));}
    ~OfflineFixture(){router.shutdownAtNativeBoundary();}
    FLStandaloneSourceRead read(int id=1){return syntheticRead(*model,id,output.data()+1);}
};
std::vector<std::string> passed;
void pass(const char* name){passed.emplace_back(name);}
FLStandaloneOverlayOutcome rtRead(FLStandaloneSourceOverlay& router,const FLStandaloneSourceRead& r){
    const auto before=overlayOfflineRTAllocations.load();overlayOfflineRT=true;const auto result=router.processObservedAfterOriginal(r);overlayOfflineRT=false;
    demand(before==overlayOfflineRTAllocations.load(),"source callback allocated");return result;
}
template<class Change>void negativeRead(const char* name,Change change,FLStandaloneOverlayFault expected){
    OfflineFixture f;auto r=f.read();change(r);const auto old=f.output;
    demand(rtRead(f.router,r)==FLStandaloneOverlayOutcome::Rejected,"targeted invalid read was accepted");
    demand(std::memcmp(f.output.data(),old.data(),sizeof(old))==0&&f.owner.fault.load(),"targeted rejection changed output or lost fixed fault queue");
    const auto stats=f.router.instanceStatsOnModelThread();demand(stats.size()==1&&!stats[0].ticketAuthorized&&stats[0].counters.rejected==1,"targeted rejection did not close its actual gate");
    const auto faults=f.router.takeFaultsOnModelThread();demand(faults.size()==1&&faults[0].instanceID=="mono"&&faults[0].firstFault==expected&&faults[0].captureSerial==1,"targeted fault metadata differs");
    demand(f.router.takeFaultsOnModelThread().empty(),"fault queue did not drain on model thread");pass(name);
}
template<class Change>void negativePublish(const char* name,Change change){
    OfflineFixture f;auto model=std::make_shared<FLNativeModelSnapshot>(*f.model);auto decoded=f.mono;auto gate=std::make_shared<StandaloneCacheGate>();
    auto cache=syntheticBatch("new",decoded,gate);change(model,decoded,cache);bool rejected=false;
    try{f.router.publish("new",decoded,model,std::move(cache));}catch(const std::exception&){rejected=true;}
    demand(rejected,"bad publication was accepted");auto r=f.read();demand(rtRead(f.router,r)==FLStandaloneOverlayOutcome::Applied,"rejected publication damaged an existing valid insert");pass(name);
}
void positiveControls(){
    OfflineFixture f;auto r=f.read();r.originalEnded=1;r.originalChanged=1;
    demand(rtRead(f.router,r)==FLStandaloneOverlayOutcome::Applied,"mono source overlay did not apply");
    for(int i=0;i<3;++i){const float expected=float(i+3)/1024.0f;demand(f.output[1+i*2]==expected&&f.output[2+i*2]==expected,"mono source was not duplicated exactly");}
    demand(f.output[0]==marker&&f.output[13]==marker&&std::all_of(f.output.begin()+7,f.output.begin()+13,[](float v){return v==marker;}),"source overlay touched native tail/guards");
    demand(r.originalReadFrames==3&&r.originalEnded==1&&r.originalChanged==1,"source overlay changed native count/flags");pass("mono duplication, exact source offset, native flags and padded tail preserved without RT allocation");
    f.output.fill(marker);r=f.read(2);demand(rtRead(f.router,r)==FLStandaloneOverlayOutcome::Applied,"cropped shared-channel region did not apply");
    demand(f.output[1]==float((12000%1003)+1)/1024.0f,"crop did not use actual native source position");
    const auto before=f.router.instanceStatsOnModelThread();demand(before[0].sources==1&&before[0].regions==2,"same native channel guessed multiple modifications");pass("two actual native region mappings share one channel modification and use their own native crop offsets");
    f.router.publish("stereo",f.stereo,f.model,syntheticBatch("stereo",f.stereo,f.stereoGate));f.output.fill(marker);r=f.read(3);
    demand(rtRead(f.router,r)==FLStandaloneOverlayOutcome::Applied,"separate mixer stereo source did not apply");
    for(int i=0;i<3;++i)demand(f.output[1+i*2]==-f.output[2+i*2],"stereo channels collapsed or reordered");pass("unique mixer instances retain separate mono/stereo source caches");
    f.output.fill(marker);r=f.read();r.nativeClipID=800;r.nativeChannelIndex=90;r.channelObject+=0x100000;putSynthetic(r.nativeRecord,32,r.nativeClipID);putSynthetic(r.nativeRecord,6,r.nativeChannelIndex);
    const auto untouched=f.output;demand(rtRead(f.router,r)==FLStandaloneOverlayOutcome::Unrelated&&f.output==untouched&&!f.owner.fault.load(),"unrelated same-sample clone was faulted or replaced");pass("unrelated native voice sharing a sample object is untouched and creates zero fault/rejection");
    auto ticket=f.monoGate->currentTicket();overlayOfflineRT=true;f.monoGate->invalidate();overlayOfflineRT=false;f.output.fill(marker);r=f.read();
    demand(!f.monoGate->authorizes(ticket)&&rtRead(f.router,r)==FLStandaloneOverlayOutcome::HeldForRebuild&&!f.owner.fault.load(),"ARA edit ticket did not hold a proven stale source");
    demand(std::all_of(f.output.begin()+1,f.output.begin()+7,[](float v){return v==0;})&&f.output[0]==marker&&f.output[13]==marker
        &&std::all_of(f.output.begin()+7,f.output.begin()+13,[](float v){return v==marker;}),"stale hold changed guards/native padding or forwarded dry positive frames");
    demand(f.owner.staleHold.exchange(false)&&f.router.takeStaleHoldsOnModelThread().size()==1,"stale source did not queue model-thread stop/rebuild-needed mailbox");
    demand(rtRead(f.router,f.read(3))==FLStandaloneOverlayOutcome::Applied,"one stale insert disabled another mixer");pass("invalidated known source holds positive frames with silence and requests stopped rebuild; separate authorized mixer remains edited");
    auto replacement=syntheticBatch("mono",f.mono,f.monoGate,2,-1);f.router.publish("mono",f.mono,f.model,std::move(replacement));f.output.fill(marker);
    demand(rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::Applied&&f.output[1]<0,"new authorized cache generation was not published");pass("new ticket/model-thread replacement publishes edited cache while retaining immutable old-batch lifetime");
    overlayOfflineRT=true;f.router.invalidateInstance("mono");overlayOfflineRT=false;f.output.fill(marker);
    demand(rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::HeldForRebuild&&std::all_of(f.output.begin()+1,f.output.begin()+7,[](float v){return v==0;}),"notification-thread invalidation did not hold matching source");
    f.router.removeInstanceAtNativeBoundary("mono");demand(rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::Unrelated,"removed insert stayed targeted");pass("allocation-free explicit invalidation and model-thread removal leave native voices original-only");
}
void zeroControls(){OfflineFixture f;auto r=f.read();r.originalReadFrames=0;r.originalEnded=1;r.originalChanged=1;
    std::fill(f.output.begin()+1,f.output.end()-1,0.0f);const auto old=f.output;
    for(const auto position:{0.0,96000.0,96512.0,std::ldexp(1.0,64)}){r.sourceStartFrame=position;
        demand(rtRead(f.router,r)==FLStandaloneOverlayOutcome::NativeZeroRead&&std::memcmp(old.data(),f.output.data(),sizeof(old))==0,"native zero-read tail changed or formed an invalid source offset");}
    const auto stats=f.router.counters();demand(stats.zeroReadCallbacks==4&&!stats.appliedFrames&&!stats.appliedCallbacks&&!stats.rejected,"native zero reads gained edited-frame credit");
    demand(r.originalReadFrames==0&&r.originalEnded==1&&r.originalChanged==1,"native zero-read flags changed");pass("EOF and ended past-EOF zero reads preserve full native output and flags with distinct zero counters and no source-pointer conversion");}
void staleHoldControls(){
    {
        OfflineFixture f;const auto checks=f.owner.holdChecks.load();f.owner.held=false;f.monoGate->invalidate();
        auto read=f.read();read.originalEnded=1;read.originalChanged=1;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::HeldForRebuild,"playing stale source did not hold");
        demand(std::all_of(f.output.begin()+1,f.output.begin()+7,[](float value){return value==0;})&&f.output[0]==marker&&f.output[13]==marker
            &&std::all_of(f.output.begin()+7,f.output.begin()+13,[](float value){return value==marker;}),"stale hold forwarded dry samples or changed output outside actual returned span");
        demand(read.originalReadFrames==3&&read.originalEnded==1&&read.originalChanged==1&&f.owner.holdChecks.load()==checks,
            "stale hold changed native flags/count or called a native-model boundary on RT");
        auto pending=f.router.takeStaleHoldsOnModelThread();demand(pending.size()==1&&pending[0].instanceID=="mono"&&pending[0].actualNativeComponent==f.mono.actualAssociation.actualComponent,
            "stale hold mailbox lost exact actual component identity");
        demand(f.owner.staleHold.exchange(false)&&f.router.takeStaleHoldsOnModelThread().empty(),"stale hold mailbox did not drain once");
        f.output.fill(marker);demand(rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::HeldForRebuild,"repeated stale read dry-forwarded");
        demand(!f.owner.staleHold.load()&&f.router.takeStaleHoldsOnModelThread().empty(),"same stale publication repeatedly queued stop requests");
        const auto stats=f.router.instanceStatsOnModelThread();demand(stats[0].staleHoldActive&&stats[0].counters.heldCallbacks==2&&stats[0].counters.heldFrames==6
            &&stats[0].counters.staleHoldRequests==1&&!stats[0].counters.appliedFrames,"stale hold counters/status falsely credited cache playback");
        f.owner.held=true;pass("proven stale positive spans hold with silence, preserve native tails/flags, coalesce one exact-owner stop request and expose counters without RT native calls");
    }
    {
        OfflineFixture f;f.monoGate->invalidate();auto read=f.read();read.originalReadFrames=0;read.originalEnded=1;read.sourceStartFrame=std::ldexp(1.0,64);
        const auto output=f.output;demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::HeldForRebuild&&f.output==output,
            "stale zero-return hold touched native EOF/tail buffer");
        const auto stats=f.router.counters();demand(stats.heldCallbacks==1&&!stats.heldFrames&&stats.zeroReadCallbacks==1&&f.owner.staleHold,
            "stale zero-return hold gained source frames or failed queue");
        pass("stale native zero reads queue rebuild-needed state without touching any native EOF/tail sample");
    }
    {
        OfflineFixture f;f.monoGate->invalidate();auto read=f.read();read.nativeClipID=800;read.nativeChannelIndex=90;read.channelObject+=0x100000;
        const auto output=f.output;demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Unrelated&&f.output==output&&!f.owner.staleHold&&!f.owner.fault,
            "stale known publication silenced an unrelated source");
        pass("unrelated and unknown sources remain byte-untouched even while another ARA publication is stale");
    }
    {
        OfflineFixture f;f.monoGate->invalidate();auto read=f.read();read.readerPCM40+=8;const auto output=f.output;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected&&f.output==output&&f.owner.fault&&!f.owner.staleHold,
            "stale hold bypassed actual reader/PCM identity seal");
        pass("stale cache never authorizes silence for a target failing pointer/class/record proof; output remains untouched and fault is queued");
    }
    {
        OfflineFixture f;f.monoGate->invalidate();rtRead(f.router,f.read());f.router.takeStaleHoldsOnModelThread();f.owner.staleHold=false;
        f.router.publish("mono",f.mono,f.model,syntheticBatch("mono",f.mono,f.monoGate,2));f.output.fill(marker);
        demand(rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::Applied&&!f.router.instanceStatsOnModelThread()[0].staleHoldActive,
            "coherent new publication did not clear a stale hold");
        f.monoGate->invalidate();f.output.fill(marker);demand(rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::HeldForRebuild&&f.owner.staleHold,
            "new stale publication did not rearm one stop request");
        const auto request=f.router.takeStaleHoldsOnModelThread();demand(request.size()==1&&request[0].captureSerial==2&&f.router.counters().staleHoldRequests==2,
            "rearmed request used old publication authority");
        pass("a coherently published edited generation clears hold and rearms at most one request for its own later invalidation");
    }
    {
        SyntheticOwner owner;auto config=syntheticConfig(owner);config.ownerStaleHoldQueued=nullptr;bool refused=false;
        try{FLStandaloneSourceOverlay missing(config);}catch(const std::exception&){refused=true;}
        demand(refused,"stale hold was enabled without its stable owner queue");
        pass("source router refuses admission without stable model-thread stale-hold mailbox");
    }
    {
        OfflineFixture f;auto retained=FLStandaloneSourceOverlayOfflineAccess::retainFirstBatch(f.router);
        f.router.publish("mono",f.mono,f.model,syntheticBatch("mono",f.mono,f.monoGate,2));
        f.owner.staleHold=false;f.output.fill(marker);overlayOfflineRT=true;
        const auto held=FLStandaloneSourceOverlayOfflineAccess::holdRetired(f.router,retained,f.read());overlayOfflineRT=false;
        demand(held==FLStandaloneOverlayOutcome::HeldForRebuild&&!f.owner.staleHold&&f.router.takeStaleHoldsOnModelThread().empty()
            &&!f.router.instanceStatsOnModelThread()[0].staleHoldActive,"retired reader re-queued STOP/status for a coherent newer publication");
        f.output.fill(marker);demand(rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::Applied,"retired hold changed the actual current ticket");
        pass("retired in-flight batch may hold its own span but cannot re-arm STOP or stale status for a newer coherent publication");
    }
}
void negativeControls(){using F=FLStandaloneOverlayFault;
    negativeRead("targeted event mutation closes matching gate and queues fixed owner fault",[](auto& r){r.nativeRecord[0]^=1;},F::RecordIdentity);
    negativeRead("targeted incomplete native observation fails closed",[](auto& r){r.nativeObservationComplete=false;},F::NativeRead);
    negativeRead("unsealed native voice class refused",[](auto& r){++r.voiceVMT;},F::ClassSeal);
    negativeRead("changed actual source object refused",[](auto& r){++r.sampleObject;},F::PhysicalIdentity);
    negativeRead("changed source PCM pointer refused",[](auto& r){++r.nativePCM90;},F::PointerRelation);
    negativeRead("reader-to-source relation refused",[](auto& r){++r.readerSample78;},F::PointerRelation);
    negativeRead("actual native rate outside 48k refused",[](auto& r){r.nativeRate=44100;},F::NativeSourceMetadata);
    negativeRead("actual native factor change refused",[](auto& r){r.nativeRateFactor=2;},F::NativeSourceMetadata);
    negativeRead("actual native frame extent change refused",[](auto& r){--r.nativeSourceFrames;},F::NativeSourceMetadata);
    negativeRead("unqualified native PCM encoding refused",[](auto& r){r.nativeFormat79=1;},F::NativeSourceMetadata);
    negativeRead("reader PCM encoding change refused",[](auto& r){r.readerFormatA1=1;},F::NativeReaderMetadata);
    negativeRead("native rate/pitch step unsupported",[](auto& r){r.readerStep=2;},F::UnsupportedSourceTransform);
    negativeRead("native streaming source unsupported",[](auto& r){r.referenceC8=1;},F::UnsupportedSourceTransform);
    negativeRead("native stretch provider unsupported",[](auto& r){r.stretchProvider100=1;},F::UnsupportedSourceTransform);
    negativeRead("output pointer must match actual native context",[](auto& r){r.outputMatchesActualNativeContext=false;},F::OutputShape);
    negativeRead("returned frame count bounded by actual block",[](auto& r){r.originalReadFrames=7;},F::OutputShape);
    negativeRead("fractional native source position refused",[](auto& r){r.sourceStartFrame=.5;},F::SourcePosition);
    negativeRead("nonfinite native source position refused",[](auto& r){r.sourceStartFrame=std::numeric_limits<double>::quiet_NaN();},F::SourcePosition);
    negativeRead("negative native source position refused",[](auto& r){r.sourceStartFrame=-1;},F::SourcePosition);
    negativeRead("positive native read past source extent refused",[](auto& r){r.sourceStartFrame=96001;},F::SourcePosition);
    negativeRead("positive native read crossing source extent refused",[](auto& r){r.sourceStartFrame=95999;},F::SourceRange);
    negativeRead("past-EOF zero read requires explicit original ended flag",[](auto& r){r.originalReadFrames=0;r.sourceStartFrame=96512;},F::SourcePosition);
    negativeRead("zero read still rejects nonfinite native position",[](auto& r){r.originalReadFrames=0;r.originalEnded=1;r.sourceStartFrame=std::numeric_limits<double>::infinity();},F::SourcePosition);
    negativeRead("zero read retains source metadata gates",[](auto& r){r.originalReadFrames=0;r.originalEnded=1;r.sourceStartFrame=96512;r.referenceC8=1;},F::UnsupportedSourceTransform);
    negativeRead("new uncaptured native clip on edited physical channel refuses implicit mapping",[](auto& r){r.nativeClipID=99;putSynthetic(r.nativeRecord,32,r.nativeClipID);},F::PhysicalIdentity);
    OfflineFixture f;f.router.publish("stereo",f.stereo,f.model,syntheticBatch("stereo",f.stereo,f.stereoGate));auto r=f.read();
    const auto stereo=syntheticRead(*f.model,3,f.output.data()+1);r.channelObject=stereo.channelObject;r.nativeChannelIndex=stereo.nativeChannelIndex;
    demand(rtRead(f.router,r)==FLStandaloneOverlayOutcome::Rejected&&!f.monoGate->authorizes(f.monoGate->currentTicket())&&
        !f.stereoGate->authorizes(f.stereoGate->currentTicket()),"ambiguous cross-instance native identity did not close all matching gates");
    demand(f.router.takeFaultsOnModelThread().size()==2,"ambiguous identity did not queue each affected instance");pass("contradictory clip/channel identities close ALL matching insert gates and queue per-instance faults");
    OfflineFixture isolated;isolated.router.publish("stereo",isolated.stereo,isolated.model,syntheticBatch("stereo",isolated.stereo,isolated.stereoGate));
    auto broken=isolated.read();broken.nativeRecord[0]^=1;
    demand(rtRead(isolated.router,broken)==FLStandaloneOverlayOutcome::Rejected&&
        rtRead(isolated.router,isolated.read(3))==FLStandaloneOverlayOutcome::Applied,"one targeted insert fault disabled another mixer");
    demand(isolated.router.takeFaultsOnModelThread().size()==1,"isolated insert failure queued an unrelated owner fault");
    pass("a targeted insert fault leaves an independently authorized mixer rendering its own source cache");
    isolated.owner.fault=false;isolated.router.publish("mono",isolated.mono,isolated.model,syntheticBatch("mono",isolated.mono,isolated.monoGate,3));
    demand(rtRead(isolated.router,isolated.read())==FLStandaloneOverlayOutcome::Applied&&!isolated.owner.fault.load(),"explicit acknowledged-fault new qualified ticket could not recover");
    pass("acknowledged native fault requires explicit new qualified publication; history remains while new ticket can recover");
}
void publicationControls(){
    negativePublish("multiple ARA instances on same mixer refused without damaging previous insert",[](auto&,auto&,auto&){});
    negativePublish("model-to-cache PCM hash disagreement refused",[](auto&,auto& d,auto&){d.sources[0].actualPCM_SHA256[0]^=1;});
    negativePublish("actual immutable source copy mutation refused",[](auto& m,auto&,auto&){m->sources[0].pcmBytes[0]^=1;});
    negativePublish("unqualified immutable model source format refused",[](auto& m,auto&,auto&){m->sources[0].nativeFormatByte79=1;});
    negativePublish("unbounded native source frame count refused before cache access",[](auto& m,auto&,auto&){m->sources[0].nativeFrameCount98=std::numeric_limits<std::int64_t>::max();});
    negativePublish("native88 snapshot disagreement refused",[](auto& m,auto&,auto&){m->clips[0].record[0]^=1;});
    negativePublish("source cache actual model generation missing refused",[](auto&,auto&,auto& b){b.captureSerial=0;});
    negativePublish("source cache model SHA differs refused",[](auto&,auto&,auto& b){b.native.sourcePCMModelSHA256=std::string(64,'d');});
    negativePublish("stale cache ticket cannot publish",[](auto&,auto&,auto& b){b.gate->invalidate();});
    negativePublish("source-only implicit modification mapping refused",[](auto&,auto&,auto& b){b.modifications[0].modificationID="wrong-modification";});
    negativePublish("cache full-source frame extent mismatch refused",[](auto&,auto&,auto& b){b.modifications[0].channels[0].pop_back();});
    negativePublish("cache nonfinite DSP output refused",[](auto&,auto&,auto& b){b.modifications[0].channels[0][0]=std::numeric_limits<float>::quiet_NaN();});
    negativePublish("cache real head/tail/latency outside fixed source route refused",[](auto&,auto&,auto& b){b.modifications[0].reportedLatencySamples=64;});
    negativePublish("cache edit-notification generation mismatch refused",[](auto&,auto&,auto& b){++b.actualEditNotificationRevision;});
    negativePublish("missing actual internal-region cleanup refused",[](auto&,auto&,auto& b){b.modifications[0].internalRegionRemoved=false;});
    OfflineFixture f;bool wrongThread=false;std::thread t([&]{try{f.router.collectRetiredOnModelThread();}catch(const std::exception&){wrongThread=true;}});t.join();demand(wrongThread,"retired table destruction allowed off model thread");
    f.owner.held=false;bool refused=false;try{f.router.removeInstanceAtNativeBoundary("mono");}catch(const std::exception&){refused=true;}f.owner.held=true;
    demand(refused,"native owner hold was optional");pass("publication/removal/retirement require actual held native owner and owning model thread");
}
void setSyntheticCapturedChannelClass(FLNativeModelSnapshot& model,std::uintptr_t offset){
    auto& channel=model.channels[0];channel.actualChannelVMT=syntheticBase+offset;
    model.nativeManagerMembers[channel.managerIndex].actualChannelVMT=channel.actualChannelVMT;
}
void observedChannelClassControls(){
    for(const auto offset:{channelVMTOffset,observedAudioChannelVMTOffset}){
        SyntheticOwner owner;FLStandaloneSourceOverlay router(syntheticConfig(owner));auto model=syntheticModel();setSyntheticCapturedChannelClass(*model,offset);
        const auto decoded=syntheticDecoded(*model,1);auto gate=std::make_shared<StandaloneCacheGate>();router.publish("class",decoded,model,syntheticBatch("class",decoded,gate));
        std::array<float,14> output;output.fill(marker);auto read=syntheticRead(*model,1,output.data()+1);
        demand(rtRead(router,read)==FLStandaloneOverlayOutcome::Applied&&output[1]==3.0f/1024.0f&&output[0]==marker&&output[13]==marker,
            "sealed base/observed-derived channel did not publish and render with exact captured VMT");
        pass(offset==channelVMTOffset?"exact sealed base channel VMT publishes and renders against its captured identity":"observed exact derived channel VMT plus complete reader inventory publishes and renders against its captured identity");
        output.fill(marker);read=syntheticRead(*model,1,output.data()+1);
        read.channelVMT=syntheticBase+(offset==channelVMTOffset?observedAudioChannelVMTOffset:channelVMTOffset);const auto before=output;
        demand(rtRead(router,read)==FLStandaloneOverlayOutcome::Rejected&&output==before&&!gate->authorizes(gate->currentTicket()),
            "accepting a known publication class weakened exact captured live VMT equality");
        const auto faults=router.takeFaultsOnModelThread();demand(faults.size()==1&&faults[0].firstFault==FLStandaloneOverlayFault::ClassSeal,
            "changed live VMT did not retain class-seal refusal");router.shutdownAtNativeBoundary();
        pass(offset==channelVMTOffset?"base publication refuses a live observed-derived VMT swap and leaves output untouched":"derived publication refuses a live base VMT swap and leaves output untouched");
    }
    {
        SyntheticOwner owner;FLStandaloneSourceOverlay router(syntheticConfig(owner));auto model=syntheticModel();setSyntheticCapturedChannelClass(*model,observedAudioChannelVMTOffset);
        const auto decoded=syntheticDecoded(*model,1);auto gate=std::make_shared<StandaloneCacheGate>();router.publish("class",decoded,model,syntheticBatch("class",decoded,gate));
        std::array<float,14> output;output.fill(marker);auto read=syntheticRead(*model,1,output.data()+1);read.channelVMT+=8;const auto before=output;
        demand(rtRead(router,read)==FLStandaloneOverlayOutcome::Rejected&&output==before&&owner.fault,"unknown live VMT was accepted after observed-derived publication");
        const auto faults=router.takeFaultsOnModelThread();demand(faults.size()==1&&faults[0].firstFault==FLStandaloneOverlayFault::ClassSeal,"unknown live class lost exact class-seal refusal");
        router.shutdownAtNativeBoundary();pass("observed-derived publication rejects unknown live VMT without allocation or output mutation");
    }
    const auto rejectClass=[](const char* name,auto change,bool numeric){
        // Fresh EMPTY router: rejection cannot be a false positive caused by
        // duplicate mixer ownership in an already published fixture.
        SyntheticOwner owner;FLStandaloneSourceOverlay router(syntheticConfig(owner));auto model=syntheticModel();setSyntheticCapturedChannelClass(*model,observedAudioChannelVMTOffset);change(*model);
        auto decoded=syntheticDecoded(*model,1);auto gate=std::make_shared<StandaloneCacheGate>();std::string refusal;
        try{router.publish("class",decoded,model,syntheticBatch("class",decoded,gate));}catch(const std::exception& error){refusal=error.what();}
        demand(refusal.find("standalone source actual physical native channel differs:")!=std::string::npos&&router.instanceStatsOnModelThread().empty(),
            "bad captured class/inventory did not fail its own physical class guard");
        if(numeric)demand(refusal.find("actual_vmt_offset=0x150c008")!=std::string::npos&&refusal.find("evidence_channel=3")!=std::string::npos
            &&refusal.find("manager_index=3")!=std::string::npos&&refusal.find("source_index=0")!=std::string::npos&&refusal.find("actual_mixer=1")!=std::string::npos
            &&refusal.find("captured_class_qualified=0")!=std::string::npos,"physical class refusal omitted actual numeric provenance");
        router.shutdownAtNativeBoundary();pass(name);
    };
    rejectClass("unknown captured channel class is refused even with matching inventory; refusal reports numeric VMT/index/mixer provenance",
        [](auto& model){setSyntheticCapturedChannelClass(model,observedAudioChannelVMTOffset+8);},true);
    rejectClass("observed-derived channel requires complete actual reader inventory",
        [](auto& model){model.allNativeManagerChannelsInspected=false;},false);
    rejectClass("observed-derived channel refuses mismatched reader member VMT",
        [](auto& model){model.nativeManagerMembers[model.channels[0].managerIndex].actualChannelVMT=syntheticBase+channelVMTOffset;},false);
    rejectClass("observed-derived channel refuses mismatched reader member object",
        [](auto& model){++model.nativeManagerMembers[model.channels[0].managerIndex].actualChannelObject;},false);
}
std::uint32_t physicalBit(FLStandalonePhysicalIdentityMismatch value){return static_cast<std::uint32_t>(value);}
void physicalDiagnosticBranchControls(){
    using Branch=FLStandalonePhysicalIdentityBranch;using M=FLStandalonePhysicalIdentityMismatch;
    {
        OfflineFixture f;auto read=f.read();read.channelObject+=0x100000;read.nativeRate=std::numeric_limits<double>::quiet_NaN();const auto output=f.output;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected&&f.output==output,"mapping diagnostic changed existing rejection/output");
        const auto stats=f.router.instanceStatsOnModelThread();demand(stats.size()==1&&stats[0].physicalIdentityDiagnosticReadyMask==1&&!stats[0].physicalIdentityDiagnosticOverflow,"first physical diagnostic not ready once");
        const auto& d=stats[0].physicalIdentityDiagnostics[0];const auto& channel=f.model->channels[0];
        demand(d.branch==Branch::MappingAgreementA&&d.slotIndex==0&&d.presenceMask==5&&d.mismatchBits==physicalBit(M::MissingPhysical)&&d.matchingBatchCount==1,
            "mapping branch/absence diagnostics conflated with late source branch");
        demand(d.entry.present&&d.entry.regionPresent&&!d.physical.present&&d.indexed.present&&d.entry.nativeRecord==read.nativeRecord
            &&d.entry.expected.nativeChannelIndex==channel.managerIndex&&d.indexed.expected.channelObject==channel.channelObject
            &&d.entry.expected.capturedChannelManager==f.model->nativeChannelManager&&d.entry.expected.capturedManagerPointerArray==f.model->nativeChannelPointerArray
            &&d.entry.expected.capturedManagerMemberObject==channel.channelObject&&d.entry.expected.capturedSourceSnapshotIndex==channel.sourceSnapshotIndex,
            "mapping diagnostic lost protected candidate/record/captured manager provenance");
        demand(d.observed.channelObject==read.channelObject&&std::isnan(d.observed.nativeRate)&&d.observed.nativeRecord==read.nativeRecord,"already observed POD/nonfinite value was transformed");
        const auto faults=f.router.takeFaultsOnModelThread();demand(faults.size()==1&&faults[0].physicalIdentityDiagnosticReadyMask==1
            &&faults[0].physicalIdentityDiagnostics[0].observed.channelObject==read.channelObject,"fault drain did not carry immutable first diagnostic");
        demand(f.router.instanceStatsOnModelThread()[0].physicalIdentityDiagnosticReadyMask==1,"fault drain reset diagnostic readiness");
        pass("PhysicalIdentity branch A records absent physical lookup, exact observed POD/record and captured manager/indexed source before fault; nonfinite doubles stay raw");
    }
    {
        OfflineFixture f;auto read=f.read();read.nativeClipID=999;putSynthetic(read.nativeRecord,32,read.nativeClipID);
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected,"missing-entry physical fault changed");
        const auto stats=f.router.instanceStatsOnModelThread();const auto& d=stats[0].physicalIdentityDiagnostics[0];
        demand(d.branch==Branch::MappingAgreementA&&d.presenceMask==6&&d.mismatchBits==physicalBit(M::MissingEntry)&&!d.entry.present
            &&d.physical.present&&d.indexed.present,"missing entry was guessed into a captured region");
        pass("PhysicalIdentity branch A distinguishes missing Playlist entry from present physical/indexed sources without classifying the voice");
    }
    {
        SyntheticOwner owner;FLStandaloneSourceOverlay router(syntheticConfig(owner));auto model=syntheticModel();model->channels[1].nativeMixerInsert248=1;
        const auto decoded=syntheticDecoded(*model,1);auto gate=std::make_shared<StandaloneCacheGate>();router.publish("two",decoded,model,syntheticBatch("two",decoded,gate));
        std::array<float,14> output;output.fill(marker);auto read=syntheticRead(*model,1,output.data()+1);read.nativeChannelIndex=model->channels[1].managerIndex;putSynthetic(read.nativeRecord,6,read.nativeChannelIndex);
        demand(rtRead(router,read)==FLStandaloneOverlayOutcome::Rejected,"same-instance wrong indexed source fault changed");
        const auto stats=router.instanceStatsOnModelThread();const auto& d=stats[0].physicalIdentityDiagnostics[0];
        demand(d.branch==Branch::MappingAgreementA&&d.presenceMask==7&&d.mismatchBits==physicalBit(M::EntryIndexedSource)
            &&d.entry.sourceAddress==d.physical.sourceAddress&&d.entry.sourceAddress!=d.indexed.sourceAddress
            &&d.entry.expected.nativeChannelIndex==3&&d.indexed.expected.nativeChannelIndex==4
            &&d.indexed.expected.capturedManagerMemberObject==model->channels[1].channelObject,"different indexed cached source/model manager was not retained separately");
        router.shutdownAtNativeBoundary();pass("PhysicalIdentity branch A preserves differing entry/physical/indexed protected sources and their own captured-manager numeric identities");
    }
    {
        OfflineFixture f;auto read=f.read();++read.sampleObject;const auto output=f.output;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected&&f.output==output,"late sample physical fault changed");
        const auto stats=f.router.instanceStatsOnModelThread();const auto& d=stats[0].physicalIdentityDiagnostics[0];
        demand(d.branch==Branch::ObservedPhysicalSourceB&&d.presenceMask==7&&d.agreementMask==63&&d.mismatchBits==physicalBit(M::SampleObject)
            &&d.observed.sampleObject==read.sampleObject&&d.entry.expected.sampleObject==f.model->sources[0].waveSampleObject
            &&d.entry.expected.sampleObject!=d.observed.sampleObject,"late actual POD mismatch not distinguished from mapping fault");
        pass("PhysicalIdentity branch B records exact observed-versus-captured sample mismatch with all mapping candidates agreeing and unchanged output");
    }
    {
        OfflineFixture f;auto read=f.read();read.nativeMixerInsert=17;read.voiceObject=0;read.readerObject=0;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected,"late missing-object/mixer physical fault changed");
        const auto stats=f.router.instanceStatsOnModelThread();const auto& d=stats[0].physicalIdentityDiagnostics[0];
        demand(d.branch==Branch::ObservedPhysicalSourceB&&d.mismatchBits==(physicalBit(M::MissingVoice)|physicalBit(M::MissingReader)|physicalBit(M::MixerInsert))
            &&d.entry.nativeMixerInsert==1&&d.observed.nativeMixerInsert==17,"late physical mismatch bit set lost a compared predicate");
        pass("PhysicalIdentity branch B retains distinct missing voice/reader and actual-versus-expected mixer values without changing guards");
    }
    {
        OfflineFixture f;auto read=f.read();read.voiceVMT++;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected&&f.router.instanceStatsOnModelThread()[0].physicalIdentityDiagnosticReadyMask==0,"nonphysical class refusal consumed first-physical slots");
        pass("non-PhysicalIdentity refusals leave all first-physical diagnostic slots untouched");
    }
}
void fixedPhysicalDiagnosticControls(){
    using Branch=FLStandalonePhysicalIdentityBranch;
    OfflineFixture f;FLStandaloneSourceOverlayOfflineAccess::seedUnrelatedLegacyCountersNearWrap(f.router);
    for(unsigned i=1;i<=4;++i){auto read=f.read();read.sampleObject+=i;read.preOriginalReaderPosition=double(i);
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected,"diagnostic reservation changed a physical refusal");
        const auto stats=f.router.instanceStatsOnModelThread();demand(stats[0].physicalIdentityDiagnosticReadyMask==((1u<<i)-1)
            &&!stats[0].physicalIdentityDiagnosticOverflow,"first-four reservation wrapped or overflowed early");
        for(unsigned j=0;j<i;++j)demand(stats[0].physicalIdentityDiagnostics[j].slotIndex==j&&stats[0].physicalIdentityDiagnostics[j].observed.preOriginalReaderPosition==double(j+1),"published slot was mixed or overwritten");
        f.router.takeFaultsOnModelThread();
    }
    for(unsigned i=0;i<1030;++i){auto read=f.read();read.sampleObject+=100+i;read.preOriginalReaderPosition=5000+i;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected,"full diagnostic mailbox altered source refusal");}
    auto stats=f.router.instanceStatsOnModelThread();demand(stats[0].physicalIdentityDiagnosticReadyMask==15&&stats[0].physicalIdentityDiagnosticOverflow,"full four-slot mailbox did not fail quietly");
    for(unsigned j=0;j<4;++j)demand(stats[0].physicalIdentityDiagnostics[j].observed.preOriginalReaderPosition==double(j+1)
        &&stats[0].physicalIdentityDiagnostics[j].branch==Branch::ObservedPhysicalSourceB,"overflow or unrelated legacy-counter wrap overwrote first four records");
    f.router.takeFaultsOnModelThread();f.router.publish("mono",f.mono,f.model,syntheticBatch("mono",f.mono,f.monoGate,2000));
    stats=f.router.instanceStatsOnModelThread();demand(stats[0].physicalIdentityDiagnosticReadyMask==15&&stats[0].physicalIdentityDiagnosticOverflow,"new publication reset/re-armed diagnostic slots");
    for(unsigned j=0;j<4;++j)demand(stats[0].physicalIdentityDiagnostics[j].observed.preOriginalReaderPosition==double(j+1),"later publication changed lifetime-Control diagnostic content");
    pass("first four immutable diagnostics survive more than 1024 overflow attempts, seeded legacy-counter wrap, fault drains and cache republication without slot reuse");
}
void concurrentPhysicalDiagnosticControls(){
    OfflineFixture f;std::atomic<unsigned> finished{0};std::atomic<bool> begin{false},bad{false};std::array<std::thread,4> workers;
    for(unsigned worker=0;worker<workers.size();++worker)workers[worker]=std::thread([&,worker]{while(!begin.load())std::this_thread::yield();overlayOfflineRT=true;
        for(unsigned i=1;i<=64;++i){const unsigned markerValue=worker*1000+i;auto read=f.read();read.voiceObject=0xa00000+markerValue;read.readerObject=0xa10000+markerValue;
            read.sampleObject+=markerValue;read.preOriginalReaderPosition=markerValue;
            if(f.router.processObservedAfterOriginal(read)!=FLStandaloneOverlayOutcome::Rejected)bad=true;}
        overlayOfflineRT=false;finished.fetch_add(1);});
    begin=true;unsigned snapshots=0;
    do{const auto stats=f.router.instanceStatsOnModelThread();for(unsigned i=0;i<4;++i)if(stats[0].physicalIdentityDiagnosticReadyMask&(1u<<i)){
            const auto& d=stats[0].physicalIdentityDiagnostics[i];const auto markerValue=static_cast<std::uintptr_t>(d.observed.preOriginalReaderPosition);
            demand(d.slotIndex==i&&markerValue&&d.observed.voiceObject==0xa00000+markerValue&&d.observed.readerObject==0xa10000+markerValue
                &&d.observed.sampleObject==d.entry.expected.sampleObject+markerValue&&d.presenceMask==7&&d.agreementMask==63
                &&d.entry.captureSerial==d.captureSerial&&d.entry.batchAddress==d.ownerBatchAddress,"acquire read observed a mixed/incomplete immutable diagnostic");}
        ++snapshots;
    }while(finished.load()!=workers.size());
    for(auto& worker:workers)worker.join();const auto stats=f.router.instanceStatsOnModelThread();
    demand(!bad&&snapshots&&stats[0].physicalIdentityDiagnosticReadyMask==15&&stats[0].physicalIdentityDiagnosticOverflow&&overlayOfflineRTAllocations.load()==0,"concurrent fixed-slot writer/status snapshot failed or allocated RT");
    pass("concurrent first-four writers and acquire-only status snapshots expose no mixed POD and overflow without any RT allocation or reset");
}
void retiredPhysicalDiagnosticControls(){
    OfflineFixture f;const auto readModelAddress=reinterpret_cast<std::uintptr_t>(f.model.get());std::weak_ptr<const FLNativeModelSnapshot> lifetime=f.model;
    auto read=f.read();read.sampleObject+=16;overlayOfflineRT=true;const auto* borrowed=FLStandaloneSourceOverlayOfflineAccess::borrow(f.router);overlayOfflineRT=false;
    f.router.invalidateInstance("mono");f.router.removeDisabledInstanceOnModelThread("mono");f.model.reset();
    demand(!lifetime.expired()&&FLStandaloneSourceOverlayOfflineAccess::retiredCount(f.router)==1,"protected retired model vanished before diagnostic copy");
    overlayOfflineRT=true;FLStandaloneSourceOverlayOfflineAccess::diagnoseBorrowedTable(f.router,borrowed,read);FLStandaloneSourceOverlayOfflineAccess::release(f.router);overlayOfflineRT=false;
    f.router.collectRetiredOnModelThread();demand(lifetime.expired(),"diagnostic retained model/batch pointers instead of numeric copies");
    const auto stats=f.router.instanceStatsOnModelThread();const auto& d=stats[0].physicalIdentityDiagnostics[0];
    demand(!stats[0].published&&stats[0].physicalIdentityDiagnosticReadyMask==1&&d.entry.expected.capturedModelAddress==readModelAddress
        &&d.observed.sampleObject==d.entry.expected.sampleObject+16&&overlayOfflineRTAllocations.load()==0,"retired-table diagnostic was lost or dereferenced after source/model release");
    pass("ReaderGuard-protected retired table yields copied numeric provenance; later status survives model destruction without retaining or dereferencing those addresses");
}
FLStandaloneSourceRead syntheticDefaultPreviewRead(const FLNativeModelSnapshot& model,float* output){
    // Actual-C9 shape is incidental non-target metadata, not a 44.1k ARA test.
    // Its clip/index aliases deliberately nominate the real first Playlist row.
    auto read=syntheticRead(model,1,output);read.channelObject=0xd00000;read.channelVMT=syntheticBase+previewChannelVMTOffset;
    read.sampleObject=0xd10000;read.readerObject=0xd20000;read.channelSample328=read.sampleObject;read.readerSample78=read.sampleObject;
    read.nativePCM90=0xd30000;read.readerPCM40=read.nativePCM90;read.nativeMixerInsert=0;read.nativeSourceFrames=90867;
    read.nativeRate=44100;read.nativeRateFactor=1;read.nativeFormat78=2;read.readerChannelsA0=2;read.readerStride48=2;read.readerStep=.91875;read.stretchProvider100=0xd40000;
    read.sourceStartFrame=0;read.nativeBlockFrames=17;read.originalReadFrames=17;read.originalEnded=1;read.originalChanged=1;
    recordDefaultPreviewOwnerObservation(read,true,read.channelObject,true,read.channelObject);return read;
}
template<class Change>void rejectUnprovedPreview(const char* name,Change change){
    OfflineFixture f;std::array<float,64> output;output.fill(marker);auto read=syntheticDefaultPreviewRead(*f.model,output.data()+1);change(read);const auto before=output;
    demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected&&output==before&&!f.monoGate->authorizes(f.monoGate->currentTicket()),"unproved preview role bypassed existing target guards");
    demand(f.router.counters().qualifiedPreviewUnrelatedCallbacks==0&&f.owner.fault,"failed preview proof credited the narrow exception or lost fault");
    const auto stats=f.router.instanceStatsOnModelThread();demand(stats[0].physicalIdentityDiagnosticReadyMask==1
        &&stats[0].physicalIdentityDiagnostics[0].branch==FLStandalonePhysicalIdentityBranch::MappingAgreementA,"unproved aliasing voice lost unchanged C9 first-physical diagnostics");
    pass(name);
}
void defaultPreviewOwnerControls(){
    {
        OfflineFixture f;std::array<float,64> output;output.fill(marker);auto read=syntheticDefaultPreviewRead(*f.model,output.data()+1);const auto before=output;const auto ticket=f.monoGate->currentTicket();
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Unrelated&&output==before&&read.originalReadFrames==17&&read.originalEnded==1&&read.originalChanged==1,
            "independently owned default preview changed original output/count/flags or hit borrowed clip aliases");
        const auto stats=f.router.instanceStatsOnModelThread();const auto totals=f.router.counters();
        demand(f.monoGate->authorizes(ticket)&&!f.owner.fault&&!f.owner.staleHold&&stats[0].physicalIdentityDiagnosticReadyMask==0&&stats[0].counters.nativeCallbacks==0
            &&totals.unrelated==1&&totals.qualifiedPreviewUnrelatedCallbacks==1&&stats[0].counters.qualifiedPreviewUnrelatedCallbacks==0,"default preview touched an insert gate/fault/diagnostic or inferred per-insert ownership");
        pass("exact current default-global Preview role bypasses borrowed clip/index aliases as original-only unrelated, preserves output/count/flags and credits only global evidence");
    }
    {
        OfflineFixture f;f.monoGate->invalidate();const auto ticket=f.monoGate->currentTicket();std::array<float,64> output;output.fill(marker);auto read=syntheticDefaultPreviewRead(*f.model,output.data()+1);const auto before=output;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Unrelated&&output==before&&f.monoGate->currentTicket().epoch==ticket.epoch
            &&!f.owner.staleHold&&!f.owner.fault&&f.router.counters().heldCallbacks==0,"stale Playlist cache silenced or faulted independently proven default preview");
        pass("proven default preview remains original-only when ARA cache is stale and never changes target gate or queues stale hold");
    }
    {
        OfflineFixture f;std::array<float,64> output;output.fill(marker);auto read=syntheticDefaultPreviewRead(*f.model,output.data()+1);read.originalReadFrames=0;const auto before=output;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Unrelated&&output==before&&read.originalEnded==1&&read.originalChanged==1&&f.router.counters().zeroReadCallbacks==0,"preview EOF/tail gained target-source credit or buffer changes");
        pass("qualified default preview zero reads retain all original tail bytes and flags without target EOF or rendered-frame credit");
    }
    rejectUnprovedPreview("Preview class and incidental mixer/PCM/record shape alone never authorize non-Playlist exclusion",[](auto& read){read.globalPreviewOwnerReadCoherent=false;});
    rejectUnprovedPreview("failed first live default-global read keeps existing strict physical identity diagnostics",[](auto& read){recordDefaultPreviewOwnerObservation(read,false,read.channelObject,true,read.channelObject);});
    rejectUnprovedPreview("failed second live default-global read keeps existing strict physical identity diagnostics",[](auto& read){recordDefaultPreviewOwnerObservation(read,true,read.channelObject,false,read.channelObject);});
    rejectUnprovedPreview("two null default-global reads do not establish an owner",[](auto& read){recordDefaultPreviewOwnerObservation(read,true,0,true,0);});
    rejectUnprovedPreview("different before/after default-global values fail closed even when final value matches observed channel",[](auto& read){recordDefaultPreviewOwnerObservation(read,true,read.channelObject+8,true,read.channelObject);});
    rejectUnprovedPreview("same-class preview pool object is not exempted by default-global authority for another object",[](auto& read){recordDefaultPreviewOwnerObservation(read,true,read.channelObject+0x1000,true,read.channelObject+0x1000);});
    rejectUnprovedPreview("unknown preview-like class is not exempted even with coherent default-global equality",[](auto& read){read.channelVMT+=8;});
    rejectUnprovedPreview("wrong native voice class is not exempted by Preview class/global equality",[](auto& read){read.voiceVMT+=8;});
    rejectUnprovedPreview("incomplete native observation remains strict despite Preview class/global equality",[](auto& read){read.nativeObservationComplete=false;});
    rejectUnprovedPreview("missing actual voice or reader object cannot gain a Preview exemption",[](auto& read){read.voiceObject=0;read.readerObject=0;});
    {
        OfflineFixture f;std::array<float,64> output;output.fill(marker);auto read=syntheticDefaultPreviewRead(*f.model,output.data()+1);read.voiceRecordRead=false;const auto before=output;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Unrelated&&output==before&&f.router.counters().qualifiedPreviewUnrelatedCallbacks==0
            &&!f.owner.fault&&f.monoGate->authorizes(f.monoGate->currentTicket()),"incomplete unidentifiable read gained Preview proof or changed legacy unrelated behavior");
        pass("missing native record follows existing unidentifiable unrelated behavior without crediting qualified Preview authority");
    }
    {
        OfflineFixture f;std::array<float,64> output;output.fill(marker);auto read=syntheticDefaultPreviewRead(*f.model,output.data()+1);
        read.channelObject=f.model->channels[0].channelObject;recordDefaultPreviewOwnerObservation(read,true,read.channelObject,true,read.channelObject);const auto before=output;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected&&output==before&&!f.monoGate->authorizes(f.monoGate->currentTicket())&&f.router.counters().qualifiedPreviewUnrelatedCallbacks==0,"captured physical pointer replacement/reuse gained Preview exclusion");
        const auto faults=f.router.takeFaultsOnModelThread();demand(faults.size()==1&&faults[0].firstFault==FLStandaloneOverlayFault::ClassSeal,"captured class replacement lost its original class-seal rejection");
        pass("captured Playlist pointer cannot be excluded even if replaced by Preview class and claimed by default global");
    }
    {
        OfflineFixture f;f.router.publish("stereo",f.stereo,f.model,syntheticBatch("stereo",f.stereo,f.stereoGate));std::array<float,64> output;output.fill(marker);
        auto read=syntheticDefaultPreviewRead(*f.model,output.data()+1);read.channelObject=f.model->channels[1].channelObject;recordDefaultPreviewOwnerObservation(read,true,read.channelObject,true,read.channelObject);const auto before=output;
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Rejected&&output==before&&!f.monoGate->authorizes(f.monoGate->currentTicket())
            &&!f.stereoGate->authorizes(f.stereoGate->currentTicket())&&f.router.counters().qualifiedPreviewUnrelatedCallbacks==0,"physical pointer owned by another captured insert was incorrectly exempted");
        pass("physical exclusion checks every captured insert, retaining strict contradictory cross-insert ownership rejection");
    }
    {
        OfflineFixture f;auto read=f.read();recordDefaultPreviewOwnerObservation(read,true,0xd00000,true,0xd00000);
        demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Applied&&f.router.counters().qualifiedPreviewUnrelatedCallbacks==0,"real Playlist source with independent preview global stopped using its exact edited cache");
        f.monoGate->invalidate();f.output.fill(marker);demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::HeldForRebuild&&f.owner.staleHold
            &&f.router.counters().qualifiedPreviewUnrelatedCallbacks==0,"real stale Playlist source escaped the existing hold via preview authority");
        pass("real Playlist and stale Playlist sources retain exact edited-cache and stale-hold behavior while default-preview proof exists independently");
    }
    {
        OfflineFixture f;FLStandaloneSourceOverlayOfflineAccess::seedQualifiedPreviewCounter(f.router,std::numeric_limits<std::uint64_t>::max()-1);
        std::array<float,64> output;output.fill(marker);auto read=syntheticDefaultPreviewRead(*f.model,output.data()+1);const auto before=output;
        for(int i=0;i<4;++i)demand(rtRead(f.router,read)==FLStandaloneOverlayOutcome::Unrelated&&output==before,"global Preview evidence counter changed unrelated processing");
        demand(f.router.counters().qualifiedPreviewUnrelatedCallbacks==std::numeric_limits<std::uint64_t>::max(),"global Preview evidence counter wrapped instead of saturating");
        pass("global-only qualified Preview evidence counter saturates near uint64 wrap without gate or output changes");
    }
}
void forwardingControl(){
    struct NativeCall{unsigned calls=0;std::uintptr_t enabled=0;void* context=nullptr;double readerPosition=17,voiceSnapshot=-1;std::array<float,8> values{};}native;
    auto original=+[](void* self,std::uintptr_t enabled,void* context,float** output,std::int32_t* count,std::uint8_t* ended,std::uint8_t* changed){
        auto& s=*static_cast<NativeCall*>(self);++s.calls;s.enabled=enabled;s.context=context;s.voiceSnapshot=s.readerPosition;s.readerPosition+=3;*output=s.values.data();*count=3;*ended=1;*changed=1;};
    float* output=nullptr;std::int32_t frames=0;std::uint8_t ended=0,changed=0;unsigned after=0;int context=42;
    forwardOnce(original,&native,7,&context,&output,&frames,&ended,&changed,[&]{++after;demand(native.calls==1&&native.voiceSnapshot==17&&native.readerPosition==20&&frames==3&&ended==1&&changed==1,"post-original source observation occurred before native snapshot/read");});
    demand(native.calls==1&&after==1&&native.enabled==7&&native.context==&context&&output==native.values.data(),"source callback forwarded original arguments more or less than once");
    pass("source ABI forwards exact original ONCE then observes original-produced pre-read position/count/flags");
}
void concurrentLifecycleControl(){
    OfflineFixture f;std::atomic<bool> run{true},unexpected{false};std::atomic<unsigned> calls{0};std::array<float,12> output{};auto read=syntheticRead(*f.model,1,output.data());
    std::thread audio([&]{overlayOfflineRT=true;while(run.load()){const auto result=f.router.processObservedAfterOriginal(read);
        if(result!=FLStandaloneOverlayOutcome::Applied&&result!=FLStandaloneOverlayOutcome::HeldForRebuild){unexpected=true;run=false;break;}calls.fetch_add(1);}overlayOfflineRT=false;});
    unsigned replacements=0;
    for(unsigned attempt=0;replacements<20&&attempt<10000;++attempt){
        auto cache=syntheticBatch("mono",f.mono,f.monoGate,2+attempt,(attempt&1)?-1.0f:1.0f);
        try{f.router.publish("mono",f.mono,f.model,std::move(cache));++replacements;}catch(const std::exception&){}
        f.router.collectRetiredOnModelThread();
    }
    run=false;audio.join();f.router.collectRetiredOnModelThread();
    demand(replacements==20&&calls>0&&!unexpected.load()&&!f.owner.fault.load()&&overlayOfflineRTAllocations.load()==0,"concurrent replacement/retirement lost immutable table or allocated RT");
    const auto counters=f.router.counters();demand(counters.readers==0&&!counters.rejected,"concurrent table lifetime did not drain");
    pass("seq_cst reader publication/retirement survives concurrent immutable table reads and model-only cache replacement without RT allocation");
}
void disabledTeardownControls(){
    {
        OfflineFixture f;f.owner.held=false;const auto checks=f.owner.holdChecks.load();const auto liveTicket=f.monoGate->currentTicket();bool refused=false;
        try{f.router.removeDisabledInstanceOnModelThread("mono");}catch(const std::exception&){refused=true;}
        demand(refused&&f.monoGate->authorizes(liveTicket)&&f.owner.holdChecks.load()==checks,
            "table-only removal closed a live ticket or required a global stopped capture");
        demand(rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::Applied,"refused live removal damaged valid playback");
        pass("table-only teardown removal refuses a live matching ticket without closing it or querying global stopped state");
        f.monoGate->invalidate();const auto closed=f.monoGate->currentTicket();f.router.removeDisabledInstanceOnModelThread("mono");
        demand(f.monoGate->currentTicket().epoch==closed.epoch&&f.owner.holdChecks.load()==checks&&
            rtRead(f.router,f.read())==FLStandaloneOverlayOutcome::Unrelated,"disabled table removal queried/mutated engine state or live gate");
        const auto stats=f.router.instanceStatsOnModelThread();demand(stats.size()==1&&!stats[0].published&&!f.owner.fault.load(),"disabled table removal lost independent stats or queued a fault");
        pass("closed-ticket immutable entry removal succeeds while synthetic global song remains playing and mutates no ticket/native state");
        f.owner.held=true;
    }
    {
        OfflineFixture f;f.monoGate->invalidate();f.owner.held=false;const auto checks=f.owner.holdChecks.load();
        f.owner.admitted=false;bool refused=false;try{f.router.removeDisabledInstanceOnModelThread("mono");}catch(const std::exception&){refused=true;}
        f.owner.admitted=true;demand(refused&&f.owner.holdChecks.load()==checks,"disabled removal allowed another/unadmitted process");
        std::atomic<bool> wrongThreadRefused{false};std::thread other([&]{try{f.router.removeDisabledInstanceOnModelThread("mono");}catch(const std::exception&){wrongThreadRefused=true;}});other.join();
        demand(wrongThreadRefused.load()&&f.owner.holdChecks.load()==checks,"disabled removal allowed non-model thread or queried engine hold");
        f.router.removeDisabledInstanceOnModelThread("mono");f.owner.held=true;
        pass("disabled entry removal still requires same admitted process and actual owning model thread");
    }
    {
        OfflineFixture f;std::weak_ptr<const FLNativeModelSnapshot> modelLifetime=f.model;std::weak_ptr<StandaloneCacheGate> gateLifetime=f.monoGate;
        overlayOfflineRT=true;const auto* borrowed=FLStandaloneSourceOverlayOfflineAccess::borrow(f.router);overlayOfflineRT=false;
        demand(borrowed&&FLStandaloneSourceOverlayOfflineAccess::regionCount(borrowed)==2,"synthetic in-flight reader failed to borrow actual immutable table");
        f.monoGate->invalidate();f.owner.held=false;const auto checks=f.owner.holdChecks.load();f.router.removeDisabledInstanceOnModelThread("mono");
        f.model.reset();f.monoGate.reset();f.router.collectRetiredOnModelThread();
        demand(!modelLifetime.expired()&&!gateLifetime.expired()&&FLStandaloneSourceOverlayOfflineAccess::retiredCount(f.router)==1&&
            FLStandaloneSourceOverlayOfflineAccess::regionCount(borrowed)==2&&FLStandaloneSourceOverlayOfflineAccess::cacheValue(borrowed)==3.0f/1024.0f&&
            f.owner.holdChecks.load()==checks,"disabled in-flight table/PCM was released or required global stopped state");
        overlayOfflineRT=true;FLStandaloneSourceOverlayOfflineAccess::release(f.router);overlayOfflineRT=false;
        demand(!modelLifetime.expired()&&!gateLifetime.expired(),"reader release destroyed vendor-independent cache storage on the audio thread");
        f.router.collectRetiredOnModelThread();demand(modelLifetime.expired()&&gateLifetime.expired()&&
            FLStandaloneSourceOverlayOfflineAccess::retiredCount(f.router)==0&&f.owner.holdChecks.load()==checks,
            "closed table did not retire on model thread after seq_cst readers drained");
        demand(overlayOfflineRTAllocations.load()==0,"in-flight table acquire/release allocated on RT path");f.owner.held=true;
        pass("disabled removal during an existing reader retains old immutable floats/model/gate until model-only collection AFTER reader drain");
    }
}
int sourceOverlayOfflineMain(){
    positiveControls();zeroControls();staleHoldControls();negativeControls();publicationControls();observedChannelClassControls();physicalDiagnosticBranchControls();fixedPhysicalDiagnosticControls();concurrentPhysicalDiagnosticControls();retiredPhysicalDiagnosticControls();defaultPreviewOwnerControls();forwardingControl();concurrentLifecycleControl();disabledTeardownControls();
    std::cout<<"{\"schema\":\"rysup.standalone-fl.source-overlay-offline/1\",\"pass\":true,\"checks\":"<<passed.size()
        <<",\"synthetic_inputs\":true,\"native_engine_loaded\":false,\"native_hook_installed\":false,\"third_party_runtime_qualified\":false,\"rt_allocations\":"<<overlayOfflineRTAllocations.load()<<",\"controls\":[";
    for(std::size_t i=0;i<passed.size();++i){if(i)std::cout<<',';std::cout<<'"'<<passed[i]<<'"';}std::cout<<"]}\n";return 0;
}
} // namespace
} // namespace rysup::ara_lab
int main(){try{return rysup::ara_lab::sourceOverlayOfflineMain();}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
#endif
