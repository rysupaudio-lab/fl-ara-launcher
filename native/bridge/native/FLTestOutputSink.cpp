#include "FLTestOutputSink.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>

namespace rysup::fl_ara::test_output {
namespace {
enum Counter {Calls,NativeCalls,NativeFrames,Nonzero,Nonfinite,Muted,Unsupported,
    Concurrent,NativeErrors,ReturnedShape,UnsafeHardware,Overflow,Registration,Start,Prepared,WriteOnly,CriticalDisarm,LifecycleRefused};
constexpr OSStatus rejected=kAudioUnitErr_CannotDoInCurrentContext;
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct Claim {
    std::atomic<bool>& state;bool held=false;
    explicit Claim(std::atomic<bool>& s):state(s){bool expected=false;held=state.compare_exchange_strong(expected,true,std::memory_order_acq_rel);}
    void release(){if(held){state.store(false,std::memory_order_release);held=false;}}
    ~Claim(){release();}
};
}
void FLTestOutputSink::bump(unsigned n,std::uint64_t amount) noexcept {
    auto previous=counters[n].load(std::memory_order_relaxed);
    if(amount>UINT64_MAX-previous){
        counters[Overflow].store(1,std::memory_order_relaxed);
        counters[n].compare_exchange_strong(previous,UINT64_MAX,std::memory_order_relaxed);return;
    }
    // One nonspinning attempt. Under simultaneous device callbacks totals are
    // monotonic lower bounds; an existing increment caused any failed attempt.
    counters[n].compare_exchange_strong(previous,previous+amount,std::memory_order_relaxed);
}
void FLTestOutputSink::invalidateRecord(Record& r,bool registration)noexcept{
    r.armed.store(false,std::memory_order_release);
    if(registration)r.registrationSucceeded.store(false,std::memory_order_release);
    const auto generation=r.preparationGeneration.load(std::memory_order_relaxed);
    if(generation==UINT64_MAX){r.retired.store(true,std::memory_order_release);bump(Overflow);}
    else r.preparationGeneration.store(generation+1,std::memory_order_release);
}
void FLTestOutputSink::OutputStartLease::release()noexcept{
    if(record_&&record_->lifecycleSlot){
        if(thread_==reinterpret_cast<std::uintptr_t>(pthread_self())){
            record_->lifecycleSlot->thread.store(0,std::memory_order_relaxed);record_->lifecycleSlot->state.store(0,std::memory_order_release);
        }else if(owner_){owner_->invalidateRecord(*record_,false);owner_->bump(LifecycleRefused);} // never release another thread's active lease
    }
    record_=nullptr;owner_=nullptr;
}
FLTestOutputSink::OutputStartLease::~OutputStartLease(){release();}
FLTestOutputSink::OutputStartLease::OutputStartLease(OutputStartLease&& o)noexcept
    :owner_(o.owner_),record_(o.record_),ticket_(o.ticket_),thread_(o.thread_){o.owner_=nullptr;o.record_=nullptr;}
FLTestOutputSink::OutputStartLease& FLTestOutputSink::OutputStartLease::operator=(OutputStartLease&& o)noexcept{
    if(this!=&o){release();owner_=o.owner_;record_=o.record_;ticket_=o.ticket_;thread_=o.thread_;o.owner_=nullptr;o.record_=nullptr;}return *this;
}
bool FLTestOutputSink::OutputStartLease::stillPrepared()const noexcept{
    return owner_&&record_&&record_->lifecycleSlot&&thread_==reinterpret_cast<std::uintptr_t>(pthread_self())&&
        record_->lifecycleSlot->state.load(std::memory_order_acquire)==1&&owner_->authorizePreparedTicket(ticket_);
}
void FLTestOutputSink::OutputStartLease::invalidateAfterFailedStart()noexcept{
    if(owner_&&record_&&thread_==reinterpret_cast<std::uintptr_t>(pthread_self()))owner_->invalidateRecord(*record_,false);
}
void FLTestOutputSink::LifecycleLease::release()noexcept{
    if(slot_&&owner_){
        if(thread_!=reinterpret_cast<std::uintptr_t>(pthread_self())){owner_->bump(LifecycleRefused);if(record_)owner_->invalidateRecord(*record_,true);}
        else {
            if(record_&&retire_)record_->retired.store(true,std::memory_order_release);
            slot_->thread.store(0,std::memory_order_relaxed);slot_->state.store(0,std::memory_order_release);
            if(!slot_->boundRecord.load(std::memory_order_acquire)){
                Claim pool(owner_->lifecyclePoolClaimed);
                if(pool.held&&!slot_->state.load(std::memory_order_acquire)&&!slot_->boundRecord.load(std::memory_order_acquire))slot_->unit.store(0,std::memory_order_release);
            }
        }
    }
    owner_=nullptr;record_=nullptr;slot_=nullptr;untracked_=false;
}
FLTestOutputSink::LifecycleLease::~LifecycleLease(){release();}
FLTestOutputSink::LifecycleLease::LifecycleLease(LifecycleLease&& o)noexcept
    :owner_(o.owner_),record_(o.record_),slot_(o.slot_),thread_(o.thread_),untracked_(o.untracked_),retire_(o.retire_){o.owner_=nullptr;o.record_=nullptr;o.slot_=nullptr;o.untracked_=false;}
FLTestOutputSink::LifecycleLease& FLTestOutputSink::LifecycleLease::operator=(LifecycleLease&& o)noexcept{
    if(this!=&o){release();owner_=o.owner_;record_=o.record_;slot_=o.slot_;thread_=o.thread_;untracked_=o.untracked_;retire_=o.retire_;o.owner_=nullptr;o.record_=nullptr;o.slot_=nullptr;o.untracked_=false;}return *this;
}
FLTestOutputSink::LifecycleLease FLTestOutputSink::tryAcquireLifecycleMutation(AudioUnit unit,bool retire,bool registration)noexcept{
    LifecycleLease out;if(!unit){out.untracked_=true;return out;}
    Claim pool(lifecyclePoolClaimed);if(!pool.held){bump(LifecycleRefused);return out;}
    const auto identity=reinterpret_cast<std::uintptr_t>(unit);LifecycleSlot* slot=nullptr;
    for(auto& candidate:lifecycleSlots)if(candidate.unit.load(std::memory_order_acquire)==identity){slot=&candidate;break;}
    if(!slot)for(auto& candidate:lifecycleSlots)if(!candidate.state.load(std::memory_order_acquire)&&!candidate.boundRecord.load(std::memory_order_acquire)){slot=&candidate;break;}
    if(!slot){bump(LifecycleRefused);return out;}
    std::uint32_t expected=0;if(!slot->state.compare_exchange_strong(expected,2,std::memory_order_acq_rel)){bump(LifecycleRefused);return out;}
    slot->unit.store(identity,std::memory_order_release);slot->thread.store(reinterpret_cast<std::uintptr_t>(pthread_self()),std::memory_order_release);
    auto* r=find(unit);if(r&&r->lifecycleSlot!=slot){slot->state.store(0,std::memory_order_release);bump(LifecycleRefused);return out;}
    out.owner_=this;out.record_=r;out.slot_=slot;out.thread_=reinterpret_cast<std::uintptr_t>(pthread_self());out.retire_=retire;
    if(r)invalidateRecord(*r,registration);return out;
}
bool FLTestOutputSink::unitMutationHeldByCurrentThread(AudioUnit unit)const noexcept{
    const auto identity=reinterpret_cast<std::uintptr_t>(unit),thread=reinterpret_cast<std::uintptr_t>(pthread_self());
    for(const auto& slot:lifecycleSlots)if(slot.unit.load(std::memory_order_acquire)==identity&&slot.state.load(std::memory_order_acquire)==2&&slot.thread.load(std::memory_order_acquire)==thread)return true;
    return false;
}
bool FLTestOutputSink::capturePreparedTicket(AudioUnit unit,PreparedOutputTicket& out)const noexcept{
    for(std::size_t i=0;i<records.size();++i){const auto& r=records[i];
        if(!r.published.load(std::memory_order_acquire)||r.unit!=unit||r.retired.load(std::memory_order_acquire))continue;
        PreparedOutputTicket ticket;ticket.unit_=unit;ticket.record_=i+1;ticket.device_=r.preparedDevice.load(std::memory_order_acquire);ticket.generation_=r.preparationGeneration.load(std::memory_order_acquire);
        if(authorizePreparedTicket(ticket)){out=ticket;return true;}return false;
    }return false;
}
bool FLTestOutputSink::authorizePreparedTicket(const PreparedOutputTicket& t)const noexcept{
    if(!t.record_||t.record_>records.size()||!t.unit_||!t.device_||!t.generation_)return false;
    const auto& r=records[t.record_-1];
    return r.published.load(std::memory_order_acquire)&&r.unit==t.unit_&&!r.retired.load(std::memory_order_acquire)&&r.lifecycleSlot&&
        r.lifecycleSlot->unit.load(std::memory_order_acquire)==reinterpret_cast<std::uintptr_t>(t.unit_)&&r.lifecycleSlot->state.load(std::memory_order_acquire)!=2&&r.armed.load(std::memory_order_acquire)&&
        r.registrationSucceeded.load(std::memory_order_acquire)&&r.preparedDevice.load(std::memory_order_acquire)==t.device_&&
        r.preparationGeneration.load(std::memory_order_acquire)==t.generation_;
}
FLTestOutputSink::OutputStartLease FLTestOutputSink::tryAcquirePreparedStart(const PreparedOutputTicket& ticket)noexcept{
    OutputStartLease out;if(!ticket.record_||ticket.record_>records.size()){bump(LifecycleRefused);return out;}
    auto& r=records[ticket.record_-1];if(!r.lifecycleSlot){bump(LifecycleRefused);return out;}std::uint32_t expected=0;
    if(!r.lifecycleSlot->state.compare_exchange_strong(expected,1,std::memory_order_acq_rel)){bump(LifecycleRefused);return out;}
    if(!authorizePreparedTicket(ticket)){r.lifecycleSlot->state.store(0,std::memory_order_release);bump(LifecycleRefused);return out;}
    const auto thread=reinterpret_cast<std::uintptr_t>(pthread_self());r.lifecycleSlot->thread.store(thread,std::memory_order_release);
    out.owner_=this;out.record_=&r;out.ticket_=ticket;out.thread_=thread;return out;
}
bool FLTestOutputSink::initialize(const Config& c) noexcept {
    Claim guard(configuring);if(!guard.held||initialized.load())return false;
    if(!c.exactAdmittedEnginePath||!*c.exactAdmittedEnginePath||
       std::strlen(c.exactAdmittedEnginePath)>=enginePath.size()||
       !c.exactTestDeviceUID||std::strcmp(c.exactTestDeviceUID,"Pro Tools Audio Bridge 2-A_UID")||
       !c.functions.setProperty||!c.functions.getProperty||!c.functions.getPropertyInfo||!c.functions.getComponent||!c.functions.getDescription||
       !c.functions.currentlyAdmitted||!c.functions.verifyExactTestDevice||
       !c.functions.currentlyAdmitted(c.functions.context))return false;
    if(!realpath(c.exactAdmittedEnginePath,enginePath.data()))return false;
    std::array<char,4096> executable{};std::uint32_t executableBytes=static_cast<std::uint32_t>(executable.size());
    if(_NSGetExecutablePath(executable.data(),&executableBytes)||!realpath(executable.data(),ownedExecutablePath.data()))return false;
    functions=c.functions;expectedOutputCallbackOffset=c.expectedOutputCallbackOffset;
    initialized.store(true,std::memory_order_release);return true;
}
bool FLTestOutputSink::isOutputUnit(AudioUnit unit,const Functions& f) noexcept {
    AudioComponentDescription d{};auto component=f.getComponent(unit);
    return component&&!f.getDescription(component,&d)&&d.componentType==kAudioUnitType_Output;
}
FLTestOutputSink::Record* FLTestOutputSink::find(AudioUnit unit) noexcept {
    for(auto& r:records)if(r.published.load(std::memory_order_acquire)&&!r.retired.load(std::memory_order_acquire)&&r.unit==unit)return &r;
    return nullptr;
}
void FLTestOutputSink::publishRegistration(const RegistrationDiagnostic& d) noexcept {
    Claim guard(diagnosticPublishing);
    if(!guard.held){
        auto previous=diagnosticDropped.load(std::memory_order_relaxed);
        if(previous!=UINT64_MAX)diagnosticDropped.compare_exchange_strong(previous,previous+1,std::memory_order_relaxed);
        return;
    }
    const std::uint64_t values[10]={static_cast<std::uint32_t>(d.stage),static_cast<std::uint32_t>(d.reason),
        d.property,d.scope,d.element,d.bytes,static_cast<std::uint32_t>(d.callbackSource),d.callbackImageOffset,
        static_cast<std::uint32_t>(d.originalStatus),(d.recordPresent?1ULL:0ULL)|(d.claimHeld?2ULL:0ULL)|(d.originalInvoked?4ULL:0ULL)};
    diagnosticSequence.fetch_add(1,std::memory_order_acq_rel);
    for(unsigned i=0;i!=10;++i)diagnosticFields[i].store(values[i],std::memory_order_relaxed);
    diagnosticSequence.fetch_add(1,std::memory_order_release);
}
RegistrationDiagnostic FLTestOutputSink::registrationSnapshot() const noexcept {
    RegistrationDiagnostic out;out.dropped=diagnosticDropped.load(std::memory_order_relaxed);
    for(unsigned attempt=0;attempt!=8;++attempt){
        const auto before=diagnosticSequence.load(std::memory_order_acquire);if(before&1)continue;
        std::uint64_t v[10];for(unsigned i=0;i!=10;++i)v[i]=diagnosticFields[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(diagnosticSequence.load(std::memory_order_acquire)!=before)continue;
        out.stage=static_cast<RegistrationStage>(v[0]);out.reason=static_cast<RegistrationReason>(v[1]);
        out.property=static_cast<std::uint32_t>(v[2]);out.scope=static_cast<std::uint32_t>(v[3]);
        out.element=static_cast<std::uint32_t>(v[4]);out.bytes=static_cast<std::uint32_t>(v[5]);
        out.callbackSource=static_cast<CallbackSource>(v[6]);out.callbackImageOffset=v[7];
        const auto bits=static_cast<std::uint32_t>(v[8]);std::memcpy(&out.originalStatus,&bits,sizeof(bits));
        out.recordPresent=(v[9]&1)!=0;out.claimHeld=(v[9]&2)!=0;out.originalInvoked=(v[9]&4)!=0;
        out.sequence=before;out.coherent=true;return out;
    }
    return out;
}
void FLTestOutputSink::publishMetadata(const CallbackMetadataDiagnostic& d) noexcept {
    Claim guard(diagnosticPublishing);
    if(!guard.held){auto previous=diagnosticDropped.load(std::memory_order_relaxed);if(previous!=UINT64_MAX)diagnosticDropped.compare_exchange_strong(previous,previous+1,std::memory_order_relaxed);return;}
    const std::uint64_t v[15]={d.registeredScope,d.registeredElement,static_cast<std::uint32_t>(d.routeGetterStatus),d.routeGetterBytes,
        static_cast<std::uint32_t>(d.routeInfoStatus),d.routeInfoBytes,d.routeWritable,static_cast<std::uint32_t>(d.inputGetterStatus),
        d.inputGetterBytes,static_cast<std::uint32_t>(d.inputInfoStatus),d.inputInfoBytes,d.inputWritable,d.outputAlias,d.accepted,
        (d.routeQueried?1ULL:0ULL)|(d.inputQueried?2ULL:0ULL)};
    metadataSequence.fetch_add(1,std::memory_order_acq_rel);
    for(unsigned i=0;i!=15;++i)metadataFields[i].store(v[i],std::memory_order_relaxed);
    metadataSequence.fetch_add(1,std::memory_order_release);
}
CallbackMetadataDiagnostic FLTestOutputSink::metadataSnapshot() const noexcept {
    CallbackMetadataDiagnostic out;out.dropped=diagnosticDropped.load(std::memory_order_relaxed);
    const auto status=[](std::uint64_t v){OSStatus value;const auto bits=static_cast<std::uint32_t>(v);std::memcpy(&value,&bits,sizeof(bits));return value;};
    for(unsigned attempt=0;attempt!=8;++attempt){
        const auto before=metadataSequence.load(std::memory_order_acquire);if(before&1)continue;
        std::uint64_t v[15];for(unsigned i=0;i!=15;++i)v[i]=metadataFields[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);if(metadataSequence.load(std::memory_order_acquire)!=before)continue;
        out.registeredScope=static_cast<std::uint32_t>(v[0]);out.registeredElement=static_cast<std::uint32_t>(v[1]);
        out.routeGetterStatus=status(v[2]);out.routeGetterBytes=static_cast<std::uint32_t>(v[3]);out.routeInfoStatus=status(v[4]);
        out.routeInfoBytes=static_cast<std::uint32_t>(v[5]);out.routeWritable=v[6];out.inputGetterStatus=status(v[7]);
        out.inputGetterBytes=static_cast<std::uint32_t>(v[8]);out.inputInfoStatus=status(v[9]);out.inputInfoBytes=static_cast<std::uint32_t>(v[10]);
        out.inputWritable=v[11];out.outputAlias=v[12];out.accepted=v[13];out.routeQueried=(v[14]&1)!=0;out.inputQueried=(v[14]&2)!=0;
        out.sequence=before;out.coherent=true;return out;
    }
    return out;
}
OSStatus FLTestOutputSink::observedSetProperty(AudioUnit unit,AudioUnitPropertyID property,
    AudioUnitScope scope,AudioUnitElement element,const void* data,UInt32 size) noexcept {
    RegistrationDiagnostic diagnostic;diagnostic.property=property;diagnostic.scope=scope;
    diagnostic.element=element;diagnostic.bytes=size;
    auto* record=find(unit);diagnostic.recordPresent=record!=nullptr;
    const auto refuse=[&](RegistrationReason reason,bool revoke=true){
        if(record&&revoke)invalidateRecord(*record,true);
        diagnostic.stage=RegistrationStage::RefusedBeforeOriginal;diagnostic.reason=reason;
        publishRegistration(diagnostic);bump(Registration);return rejected;
    };
    if(!initialized.load(std::memory_order_acquire))return refuse(RegistrationReason::NotInitialized);
    if(property!=kAudioUnitProperty_SetRenderCallback){
        if(property==kAudioUnitProperty_StreamFormat||property==kAudioUnitProperty_MaximumFramesPerSlice||
           property==kAudioOutputUnitProperty_CurrentDevice||property==kAudioOutputUnitProperty_EnableIO||
           property==kAudioOutputUnitProperty_ChannelMap||property==kAudioUnitProperty_AudioChannelLayout||
           property==kAudioUnitProperty_ElementCount){
            auto mutation=tryAcquireLifecycleMutation(unit);
            if(!mutation)return rejected;
            if(record)bump(CriticalDisarm);
            return functions.setProperty(unit,property,scope,element,data,size);
        }
        return functions.setProperty(unit,property,scope,element,data,size);
    }
    auto mutation=tryAcquireLifecycleMutation(unit,false,true);
    if(!mutation)return refuse(RegistrationReason::LifecycleBusy,false);
    record=mutation.record_;diagnostic.recordPresent=record!=nullptr; // leased authority, not pre-acquire snapshot
    if(!functions.currentlyAdmitted(functions.context))return refuse(RegistrationReason::NotAdmitted);
    if(!isOutputUnit(unit,functions))return functions.setProperty(unit,property,scope,element,data,size);
    Claim guard(configuring);diagnostic.claimHeld=guard.held;
    if(!guard.held)return refuse(RegistrationReason::ConfigurationBusy);
    if(scope!=kAudioUnitScope_Input&&scope!=kAudioUnitScope_Output)return refuse(RegistrationReason::UnsupportedScope);
    if(element!=0)return refuse(RegistrationReason::UnsupportedElement);
    if(!data)return refuse(RegistrationReason::MissingData);
    if(size!=sizeof(AURenderCallbackStruct))return refuse(RegistrationReason::WrongDataBytes);
    auto requested=*static_cast<const AURenderCallbackStruct*>(data);
    if(!requested.inputProc){
        if(record)invalidateRecord(*record,true);
        guard.release();
        diagnostic.originalInvoked=true;
        diagnostic.originalStatus=functions.setProperty(unit,property,scope,element,data,size);
        diagnostic.stage=diagnostic.originalStatus?RegistrationStage::OriginalFailed:RegistrationStage::Removed;
        if(diagnostic.originalStatus){diagnostic.reason=RegistrationReason::OriginalSetterFailed;bump(Registration);}
        publishRegistration(diagnostic);return diagnostic.originalStatus;
    }
    Dl_info image{};std::array<char,4096> imagePath{};
    if(reinterpret_cast<std::uintptr_t>(requested.inputProc)==reinterpret_cast<std::uintptr_t>(render)){
        diagnostic.callbackSource=CallbackSource::SelfWrapper;return refuse(RegistrationReason::SelfCallback);
    }
    if(!dladdr(reinterpret_cast<void*>(requested.inputProc),&image)||!image.dli_fname){
        diagnostic.callbackSource=CallbackSource::NoImage;return refuse(RegistrationReason::NoCallbackImage);
    }
    diagnostic.callbackSource=CallbackSource::OtherImage;
    const auto address=reinterpret_cast<std::uintptr_t>(requested.inputProc),base=reinterpret_cast<std::uintptr_t>(image.dli_fbase);
    if(address>=base)diagnostic.callbackImageOffset=address-base;
    if(!realpath(image.dli_fname,imagePath.data()))return refuse(RegistrationReason::CallbackPathUnresolved);
    if(!std::strcmp(imagePath.data(),enginePath.data()))diagnostic.callbackSource=CallbackSource::ExactEngine;
    else if(!std::strcmp(imagePath.data(),ownedExecutablePath.data()))diagnostic.callbackSource=CallbackSource::OwnedExecutable;
    if(diagnostic.callbackSource!=CallbackSource::ExactEngine)return refuse(RegistrationReason::CallbackNotEngine);
    if(scope==kAudioUnitScope_Output&&expectedOutputCallbackOffset&&diagnostic.callbackImageOffset!=expectedOutputCallbackOffset)return refuse(RegistrationReason::CallbackOffsetMismatch);
    if(record&&record->original.inputProc!=requested.inputProc)return refuse(RegistrationReason::CallbackChanged);
    if(record&&record->original.inputProcRefCon!=requested.inputProcRefCon)return refuse(RegistrationReason::RefconChanged);
    if(record&&(record->registeredScope!=scope||record->registeredElement!=element))return refuse(RegistrationReason::RegistrationRouteChanged);
    if(!record){
        for(auto& candidate:records)if(!candidate.published.load(std::memory_order_acquire)){record=&candidate;break;}
        if(!record)return refuse(RegistrationReason::PoolExhausted);
        record->unit=unit;record->owner=this;record->original=requested;
        record->lifecycleSlot=mutation.slot_;mutation.record_=record;mutation.slot_->boundRecord.store(true,std::memory_order_release);
        record->registeredScope=scope;record->registeredElement=element;
        record->published.store(true,std::memory_order_release);
    }
    if(!mutation.owner_){mutation=tryAcquireLifecycleMutation(unit,false,true);if(!mutation)return refuse(RegistrationReason::LifecycleBusy,false);}
    AURenderCallbackStruct wrapped{render,record};
    guard.release(); // no pool/table claim spans a foreign API call
    record->registrationSucceeded.store(false,std::memory_order_release);diagnostic.originalInvoked=true;
    diagnostic.originalStatus=functions.setProperty(unit,property,scope,element,&wrapped,sizeof(wrapped));
    if(diagnostic.originalStatus){record->armed.store(false,std::memory_order_release);bump(Registration);diagnostic.stage=RegistrationStage::OriginalFailed;diagnostic.reason=RegistrationReason::OriginalSetterFailed;}
    else {record->registrationSucceeded.store(true,std::memory_order_release);diagnostic.stage=RegistrationStage::Registered;}
    publishRegistration(diagnostic);return diagnostic.originalStatus;
}
bool FLTestOutputSink::validFormat(const AudioStreamBasicDescription& f) noexcept {
    constexpr auto allowed=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved;
    if(f.mSampleRate!=48000.0||f.mFormatID!=kAudioFormatLinearPCM||
       (f.mFormatFlags&~allowed)||!(f.mFormatFlags&kAudioFormatFlagIsFloat)||
       !(f.mFormatFlags&kAudioFormatFlagIsPacked)||f.mFramesPerPacket!=1||
       (f.mChannelsPerFrame!=1&&f.mChannelsPerFrame!=2)||
       (f.mBitsPerChannel!=32&&f.mBitsPerChannel!=64))return false;
    const auto bytes=(f.mBitsPerChannel/8)*((f.mFormatFlags&kAudioFormatFlagIsNonInterleaved)?1:f.mChannelsPerFrame);
    return f.mBytesPerFrame==bytes&&f.mBytesPerPacket==bytes;
}
bool FLTestOutputSink::sameFormat(const AudioStreamBasicDescription& a,const AudioStreamBasicDescription& b) noexcept {
    return a.mSampleRate==b.mSampleRate&&a.mFormatID==b.mFormatID&&a.mFormatFlags==b.mFormatFlags&&
        a.mBytesPerPacket==b.mBytesPerPacket&&a.mFramesPerPacket==b.mFramesPerPacket&&
        a.mBytesPerFrame==b.mBytesPerFrame&&a.mChannelsPerFrame==b.mChannelsPerFrame&&a.mBitsPerChannel==b.mBitsPerChannel;
}
bool FLTestOutputSink::tryPrepareMutedOutputStart(AudioUnit unit) noexcept {
    CallbackMetadataDiagnostic metadata;
    const auto refuse=[&](Record* r){if(r)r->armed.store(false,std::memory_order_release);publishMetadata(metadata);bump(Start);return false;};
    if(!initialized.load(std::memory_order_acquire))return refuse(nullptr);
    auto mutation=tryAcquireLifecycleMutation(unit);
    auto* r=mutation.record_;
    if(!mutation){publishMetadata(metadata);bump(Start);return false;}
    if(!r||!r->registrationSucceeded.load(std::memory_order_acquire)||!functions.currentlyAdmitted(functions.context)||!isOutputUnit(unit,functions))return refuse(r);
    metadata.registeredScope=r->registeredScope;metadata.registeredElement=r->registeredElement;
    AURenderCallbackStruct callback{};UInt32 size=sizeof(callback);
    metadata.routeQueried=true;
    metadata.routeGetterStatus=functions.getProperty(unit,kAudioUnitProperty_SetRenderCallback,r->registeredScope,r->registeredElement,&callback,&size);
    metadata.routeGetterBytes=size;
    UInt32 infoBytes=0;Boolean writable=false;
    metadata.routeInfoStatus=functions.getPropertyInfo(unit,kAudioUnitProperty_SetRenderCallback,r->registeredScope,r->registeredElement,&infoBytes,&writable);
    metadata.routeInfoBytes=infoBytes;metadata.routeWritable=writable;
    bool writeOnly=false;
    if(r->registeredScope==kAudioUnitScope_Output){
        // Measured native FL route: the actual Output0 setter succeeds, but
        // Apple AUHAL rejects Output0 getter/info. Documented Input0 metadata
        // describes the same write-only property; never claim callback readback.
        if(metadata.routeGetterStatus!=kAudioUnitErr_InvalidScope||metadata.routeGetterBytes!=sizeof(callback)||
           metadata.routeInfoStatus!=kAudioUnitErr_InvalidScope||metadata.routeInfoBytes!=0||metadata.routeWritable)return refuse(r);
        AURenderCallbackStruct inputCallback{};size=sizeof(inputCallback);
        metadata.inputQueried=true;
        metadata.inputGetterStatus=functions.getProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&inputCallback,&size);
        metadata.inputGetterBytes=size;infoBytes=0;writable=false;
        metadata.inputInfoStatus=functions.getPropertyInfo(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&infoBytes,&writable);
        metadata.inputInfoBytes=infoBytes;metadata.inputWritable=writable;
        if(metadata.inputGetterStatus!=kAudioUnitErr_InvalidProperty||metadata.inputGetterBytes!=0||
           metadata.inputInfoStatus||metadata.inputInfoBytes!=sizeof(AURenderCallbackStruct)||!metadata.inputWritable)return refuse(r);
        metadata.outputAlias=true;writeOnly=true;
    }else if(!metadata.routeGetterStatus){
        if(size!=sizeof(callback)||reinterpret_cast<std::uintptr_t>(callback.inputProc)!=reinterpret_cast<std::uintptr_t>(render)||callback.inputProcRefCon!=r||
           metadata.routeInfoStatus||metadata.routeInfoBytes!=sizeof(callback)||!metadata.routeWritable)return refuse(r);
    }else{
        if(metadata.routeGetterStatus!=kAudioUnitErr_InvalidProperty||metadata.routeGetterBytes!=0||metadata.routeInfoStatus||
           metadata.routeInfoBytes!=sizeof(AURenderCallbackStruct)||!metadata.routeWritable)return refuse(r);
        writeOnly=true;
    }
    if(writeOnly){
        AudioComponentDescription component{};const auto actualComponent=functions.getComponent(unit);
        if(!actualComponent||functions.getDescription(actualComponent,&component)||component.componentType!=kAudioUnitType_Output||
           component.componentManufacturer!=kAudioUnitManufacturer_Apple)return refuse(r);
    }
    metadata.accepted=true;
    AudioDeviceID device=0;size=sizeof(device);
    if(functions.getProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,&size)||
       size!=sizeof(device)||!functions.verifyExactTestDevice(device,functions.context))return refuse(r);
    AudioStreamBasicDescription format{};size=sizeof(format);
    if(functions.getProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,&size)||
       size!=sizeof(format)||!validFormat(format))return refuse(r);
    UInt32 maximum=0;size=sizeof(maximum);
    if(functions.getProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,&size)||
       size!=sizeof(maximum)||!maximum||maximum>maximumFrames)return refuse(r);
    if(r->preparedBefore){if(!sameFormat(r->format,format)||r->maximumSliceFrames!=maximum)return refuse(r);}
    else {r->format=format;r->maximumSliceFrames=maximum;r->preparedDevice.store(device,std::memory_order_relaxed);r->preparedBefore=true;bump(Prepared);}
    if(r->preparedDevice.load(std::memory_order_relaxed)!=device)return refuse(r);
    if(writeOnly)bump(WriteOnly);
    publishMetadata(metadata);
    r->armed.store(true,std::memory_order_release);return true;
}
bool FLTestOutputSink::zeroHardware(AudioBufferList* buffers,AudioUnitRenderActionFlags* flags) noexcept {
    if(flags)*flags|=kAudioUnitRenderAction_OutputIsSilence;
    if(!buffers||buffers->mNumberBuffers>2){bump(UnsafeHardware);return false;}
    std::size_t total=0;bool safe=true;
    for(UInt32 i=0;i<buffers->mNumberBuffers;++i){
        const auto& b=buffers->mBuffers[i];
        if(b.mDataByteSize>maximumZeroBytes-total||(!b.mData&&b.mDataByteSize)){safe=false;continue;}
        total+=b.mDataByteSize;if(b.mDataByteSize)std::memset(b.mData,0,b.mDataByteSize);
    }
    if(!safe)bump(UnsafeHardware);return safe;
}
bool FLTestOutputSink::hardwareShape(const Record& r,UInt32 frames,const AudioBufferList* buffers) const noexcept {
    if(!buffers||frames>r.maximumSliceFrames||frames>maximumFrames)return false;
    const bool planar=(r.format.mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0;
    const UInt32 count=planar?r.format.mChannelsPerFrame:1;
    if(buffers->mNumberBuffers!=count)return false;
    const UInt32 channels=planar?1:r.format.mChannelsPerFrame;
    const UInt32 bytes=frames*r.format.mBytesPerFrame;
    for(UInt32 i=0;i<count;++i){const auto& b=buffers->mBuffers[i];
        if(b.mNumberChannels!=channels||b.mDataByteSize!=bytes||(!b.mData&&bytes))return false;
        auto p=reinterpret_cast<std::uintptr_t>(b.mData),begin=reinterpret_cast<std::uintptr_t>(r.scratch.data());
        if(bytes&&p>=begin&&p<begin+scratchBytes)return false;
    }
    return true;
}
bool FLTestOutputSink::returnedShape(const Record& r,UInt32 frames) const noexcept {
    const bool planar=(r.format.mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0;
    const UInt32 count=planar?r.format.mChannelsPerFrame:1;
    if(r.list.mNumberBuffers!=count)return false;
    for(UInt32 i=0;i<count;++i){const auto& b=r.list.mBuffers[i];
        if(b.mNumberChannels!=(planar?1:r.format.mChannelsPerFrame)||b.mDataByteSize!=frames*r.format.mBytesPerFrame||(!b.mData&&b.mDataByteSize))return false;
    }
    return true;
}
void FLTestOutputSink::inspectScratch(const Record& r,UInt32 frames) noexcept {
    std::uint64_t nonzero=0,nonfinite=0;
    for(UInt32 b=0;b<r.list.mNumberBuffers;++b){const auto& buffer=r.list.mBuffers[b];
        const auto count=frames*buffer.mNumberChannels;
        for(UInt32 n=0;n<count;++n){double v;
            if(r.format.mBitsPerChannel==32){float f;std::memcpy(&f,static_cast<const std::byte*>(buffer.mData)+n*4,4);v=f;}
            else std::memcpy(&v,static_cast<const std::byte*>(buffer.mData)+n*8,8);
            if(v!=0)++nonzero;if(!std::isfinite(v))++nonfinite;
        }
    }
    bump(Nonzero,nonzero);bump(Nonfinite,nonfinite);
}
OSStatus FLTestOutputSink::render(void* context,AudioUnitRenderActionFlags* flags,const AudioTimeStamp* timestamp,
    UInt32 bus,UInt32 frames,AudioBufferList* hardware) noexcept {
    auto& r=*static_cast<Record*>(context);auto& sink=*r.owner;sink.bump(Calls);
    if(!flags||!timestamp||!r.armed.load(std::memory_order_acquire)||!sink.hardwareShape(r,frames,hardware)){
        sink.bump(Unsupported);sink.zeroHardware(hardware,flags);return rejected;
    }
    Claim guard(r.rendering);
    if(!guard.held){sink.bump(Concurrent);sink.zeroHardware(hardware,flags);return rejected;}
    // Scratch descriptors are rebuilt every call. The original may return its
    // own internal sample pointer; no returned pointer is ever cleared.
    const bool planar=(r.format.mFormatFlags&kAudioFormatFlagIsNonInterleaved)!=0;
    const UInt32 count=planar?r.format.mChannelsPerFrame:1,bytes=frames*r.format.mBytesPerFrame;
    r.list.mNumberBuffers=count;
    std::memset(r.scratch.data(),0,static_cast<std::size_t>(bytes)*count);
    for(UInt32 i=0;i<count;++i)r.list.mBuffers[i]={planar?1:r.format.mChannelsPerFrame,bytes,r.scratch.data()+static_cast<std::size_t>(i)*bytes};
    sink.bump(NativeCalls);
    // Preserve only the original driver's descriptors. Native returned sample
    // aliases live in r.list and can never become a memset target here.
    List2 originalHardware;originalHardware.mNumberBuffers=hardware->mNumberBuffers;
    for(UInt32 i=0;i<originalHardware.mNumberBuffers;++i)originalHardware.mBuffers[i]=hardware->mBuffers[i];
    const auto status=r.original.inputProc(r.original.inputProcRefCon,flags,timestamp,bus,frames,reinterpret_cast<AudioBufferList*>(&r.list));
    const bool okay=sink.returnedShape(r,frames);
    if(status)sink.bump(NativeErrors);
    if(!okay)sink.bump(ReturnedShape);
    if(!status&&okay){sink.bump(NativeFrames,frames);sink.inspectScratch(r,frames);}
    const bool zeroed=sink.zeroHardware(reinterpret_cast<AudioBufferList*>(&originalHardware),flags);
    hardware->mNumberBuffers=originalHardware.mNumberBuffers;
    for(UInt32 i=0;i<originalHardware.mNumberBuffers;++i)hardware->mBuffers[i]=originalHardware.mBuffers[i];
    if(zeroed)sink.bump(Muted,frames);
    return status?status:((okay&&zeroed)?noErr:rejected);
}
Stats FLTestOutputSink::snapshot() const noexcept {
    std::uint64_t v[18];for(unsigned i=0;i!=18;++i)v[i]=counters[i].load(std::memory_order_relaxed);
    return {v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17],registrationSnapshot(),metadataSnapshot()};
}
} // namespace rysup::fl_ara::test_output
