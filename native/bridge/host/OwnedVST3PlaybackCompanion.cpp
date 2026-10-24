#include "OwnedVST3PlaybackCompanion.h"
#include "VST3ARAEntry.h"
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <pluginterfaces/base/ibstream.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace rysup::ara_lab {
namespace {
thread_local unsigned ownedCallDepth=0;
struct OwnedCall {OwnedCall(){++ownedCallDepth;}~OwnedCall(){--ownedCallDepth;}};
void demand(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T> std::uintptr_t address(T* value){return reinterpret_cast<std::uintptr_t>(value);}
class OriginalOrdinaryStateStream final : public Steinberg::IBStream,public Steinberg::ISizeableStream {
    std::atomic<Steinberg::uint32> references{1};
    Bytes bytes;
    Steinberg::int64 position=0;
public:
    explicit OriginalOrdinaryStateStream(const Bytes& actual):bytes(actual){}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,void** out)override{
        if(!out)return Steinberg::kInvalidArgument;*out=nullptr;
        if(Steinberg::FUnknownPrivate::iidEqual(iid,INLINE_UID_OF(Steinberg::IBStream)) || Steinberg::FUnknownPrivate::iidEqual(iid,INLINE_UID_OF(Steinberg::FUnknown)))*out=static_cast<Steinberg::IBStream*>(this);
        else if(Steinberg::FUnknownPrivate::iidEqual(iid,INLINE_UID_OF(Steinberg::ISizeableStream)))*out=static_cast<Steinberg::ISizeableStream*>(this);
        else return Steinberg::kNoInterface;addRef();return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API addRef()override{return ++references;}
    Steinberg::uint32 PLUGIN_API release()override{const auto count=--references;if(!count)delete this;return count;}
    Steinberg::tresult PLUGIN_API read(void* out,Steinberg::int32 count,Steinberg::int32* actual)override{
        if(actual)*actual=0;if(count<0 || (!out && count) || position<0 || static_cast<std::uint64_t>(position)>bytes.size())return Steinberg::kInvalidArgument;
        const auto amount=std::min<Steinberg::int64>(count,static_cast<Steinberg::int64>(bytes.size())-position);
        if(amount)std::memcpy(out,bytes.data()+position,static_cast<std::size_t>(amount));position+=amount;if(actual)*actual=static_cast<Steinberg::int32>(amount);
        return amount || !count?Steinberg::kResultOk:Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API write(void*,Steinberg::int32,Steinberg::int32* written)override{if(written)*written=0;return Steinberg::kNotImplemented;}
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 offset,Steinberg::int32 mode,Steinberg::int64* actual)override{
        if(mode<0 || mode>2)return Steinberg::kInvalidArgument;
        const auto base=mode==0?0:mode==1?position:static_cast<Steinberg::int64>(bytes.size());
        if(offset < -base || offset>static_cast<Steinberg::int64>(bytes.size())-base)return Steinberg::kInvalidArgument;
        position=base+offset;if(actual)*actual=position;return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* out)override{if(!out)return Steinberg::kInvalidArgument;*out=position;return Steinberg::kResultOk;}
    Steinberg::tresult PLUGIN_API getStreamSize(Steinberg::int64& out)override{out=static_cast<Steinberg::int64>(bytes.size());return Steinberg::kResultOk;}
    Steinberg::tresult PLUGIN_API setStreamSize(Steinberg::int64)override{return Steinberg::kNotImplemented;}
};
}
bool ownedPlaybackCompanionCallOnThisThread()noexcept{return ownedCallDepth!=0;}
struct OwnedVST3PlaybackCompanion::Impl {
    OwnedVST3PlaybackFactoryContext context;
    Steinberg::IPluginFactory* retainedFactory=nullptr;
    ARA::IMainFactory* provider=nullptr;
    Steinberg::Vst::IComponent* component=nullptr;
    Steinberg::Vst::IAudioProcessor* processor=nullptr;
    std::unique_ptr<VST3ARAEntry> entry;
    std::thread::id modelThread=std::this_thread::get_id();
    const ARA::ARAFactory* expectedFactory=nullptr;
    std::uintptr_t componentIdentity=0;
    bool initialized=false,bound=false,fresh=true,active=false,processing=false,uncertain=false,disposed=false;
    bool activationEver=false,ordinaryStateRestored=false;
    bool inputBusEnabled=false,outputBusEnabled=false;
    int inputBusCount=0;
    int channels=0,maximumBlock=0,processMode=Steinberg::Vst::kOffline;
    explicit Impl(const OwnedVST3PlaybackFactoryContext& c):context(c) {
        demand(context.actualSameNativeFactory && context.alreadyLoadedModuleLifetime && context.initializedFactoryLease,
               "same native factory/module and initialized ARA lease required for owned playback");
        context.initializedFactoryLease->requireModelThread();expectedFactory=context.initializedFactoryLease->get();
        retainedFactory=context.actualSameNativeFactory;retainedFactory->addRef();
    }
    void checkThread()const{demand(modelThread==std::this_thread::get_id(),"owned playback requires its model thread");context.initializedFactoryLease->requireModelThread();}
    void construct() {
        OwnedCall scope;checkThread();
        Steinberg::PClassInfo audio{},ara{};bool audioFound=false,araFound=false;
        for(Steinberg::int32 i=0;i<retainedFactory->countClasses();++i){Steinberg::PClassInfo info{};
            demand(retainedFactory->getClassInfo(i,&info)==Steinberg::kResultOk,"owned class metadata lookup failed");
            if(!std::memcmp(info.cid,context.audioClassID.data(),16)){audio=info;audioFound=true;}
            if(!std::memcmp(info.cid,context.araClassID.data(),16)){ara=info;araFound=true;}
        }
        demand(audioFound && araFound && !std::strcmp(audio.category,kVstAudioEffectClass)
            && !std::strcmp(ara.category,kARAMainFactoryClass) && !std::strcmp(audio.name,ara.name),
            "owned playback class IDs do not match actual native audio/ARA classes");
        demand(retainedFactory->createInstance(ara.cid,INLINE_UID_OF(ARA::IMainFactory),reinterpret_cast<void**>(&provider))==Steinberg::kResultOk && provider,
               "same native factory did not create owned independent ARA provider");
        demand(provider->getFactory()==expectedFactory,"owned playback provider is not same initialized ARA factory");
        demand(retainedFactory->createInstance(audio.cid,INLINE_UID_OF(Steinberg::Vst::IComponent),reinterpret_cast<void**>(&component))==Steinberg::kResultOk && component,
               "same native factory did not create owned fresh VST3 component");
        componentIdentity=address(component);
        demand(!context.forbiddenBorrowedNativeComponent || componentIdentity!=context.forbiddenBorrowedNativeComponent,
               "factory returned borrowed native component instead of a new owned child");
        demand(component->initialize(nullptr)==Steinberg::kResultOk,"owned VST3 initialize failed");initialized=true;
        demand(component->queryInterface(INLINE_UID_OF(Steinberg::Vst::IAudioProcessor),reinterpret_cast<void**>(&processor))==Steinberg::kResultOk && processor,
               "owned VST3 lacks actual audio processor interface");
        VST3ARAEntry::NativeLifecycle lifecycle;
        lifecycle.freshBeforeFirstBinding=[this]{return fresh && !bound && initialized && !disposed;};
        lifecycle.inactive=[this]{return !active && !processing && !uncertain;};
        lifecycle.saveState=[]()->Bytes{throw std::logic_error("transient owned playback state is not a native project archive");};
        lifecycle.restoreState=[](const Bytes&){throw std::logic_error("owned playback child must use shared ARA document, not copied component state");};
        entry=std::make_unique<VST3ARAEntry>(*component,*provider,context.alreadyLoadedModuleLifetime,std::move(lifecycle));
    }
    ~Impl() {
        OwnedCall scope;
        // Normal checked disposal runs before this destructor. Failed disposal
        // is preserved by the outer owner rather than deleting uncertain DSP.
        if(processor)processor->release();
        if(component)component->release();
        if(provider)provider->release();
        if(retainedFactory)retainedFactory->release();
    }
};
OwnedVST3PlaybackCompanion::OwnedVST3PlaybackCompanion(std::unique_ptr<Impl> value):p(std::move(value)){}
std::unique_ptr<OwnedVST3PlaybackCompanion> OwnedVST3PlaybackCompanion::create(const OwnedVST3PlaybackFactoryContext& context) {
    auto result=std::unique_ptr<OwnedVST3PlaybackCompanion>(new OwnedVST3PlaybackCompanion(std::make_unique<Impl>(context)));
    result->p->construct();return result;
}
OwnedVST3PlaybackCompanion::~OwnedVST3PlaybackCompanion() {
    if(!p)return;
    try {disposeOwnedPlayback();}
    catch(...){auto* preserved=p.release();(void)preserved;std::fputs("Owned ARA playback child disposal failed; own lifecycle/module preserved\n",stderr);}
}
const ARA::ARAFactory* OwnedVST3PlaybackCompanion::factory()const noexcept{return p->expectedFactory;}
std::shared_ptr<void> OwnedVST3PlaybackCompanion::factoryLifetime()const{return p->entry?p->entry->factoryLifetime():p->context.initializedFactoryLease;}
const ARA::ARAPlugInExtensionInstance* OwnedVST3PlaybackCompanion::bind(ARA::ARADocumentControllerRef dc,ARA::ARAPlugInInstanceRoleFlags assigned) {
    OwnedCall scope;p->checkThread();demand(assigned==ARA::kARAPlaybackRendererRole && p->fresh && !p->bound,"owned cache child binds Playback-only exactly once");
    try {auto result=p->entry->bind(dc,assigned);p->fresh=false;p->bound=result!=nullptr;demand(p->bound,"owned playback ARA binding failed");return result;}
    catch(...){p->fresh=false;throw;}
}
bool OwnedVST3PlaybackCompanion::inactive()const noexcept{return !p->active && !p->processing && !p->uncertain;}
Bytes OwnedVST3PlaybackCompanion::saveState(){throw std::logic_error("owned transient playback has no independent persistent companion state");}
void OwnedVST3PlaybackCompanion::restoreState(const Bytes& actualOriginalOrdinaryState){
    OwnedCall scope;p->checkThread();
    demand(p->bound && p->initialized && !p->disposed && inactive() && !p->activationEver && !p->ordinaryStateRestored
        && actualOriginalOrdinaryState.size()<=64u*1024u*1024u,"owned ordinary state must follow genuine bind, once before first activation");
    auto* stream=new OriginalOrdinaryStateStream(actualOriginalOrdinaryState);
    const auto result=p->component->setState(stream);stream->release();
    demand(result==Steinberg::kResultOk,"actual original-only ordinary state was refused by owned playback component");
    p->ordinaryStateRestored=true;
}
std::uintptr_t OwnedVST3PlaybackCompanion::ownedComponentAddress()const noexcept{return p->componentIdentity;}
void OwnedVST3PlaybackCompanion::startOwnedPlayback(double sampleRate,int channels,int block,bool offline) {
    OwnedCall scope;p->checkThread();demand(p->bound && !p->disposed && inactive() && sampleRate==48000.0 && (channels==1 || channels==2) && block>0 && block<=4096,
                                        "owned playback setup must be bound/inactive, exact 48k and admitted channel/block layout");
    auto* processor=p->processor;const int inputs=p->component->getBusCount(Steinberg::Vst::kAudio,Steinberg::Vst::kInput);
    const int outputs=p->component->getBusCount(Steinberg::Vst::kAudio,Steinberg::Vst::kOutput);
    demand(inputs>=0 && inputs<=1 && outputs==1,"owned playback prototype supports zero/one input bus and one output bus only");
    p->processMode=offline?Steinberg::Vst::kOffline:Steinberg::Vst::kRealtime;
    Steinberg::Vst::ProcessSetup setup {p->processMode,Steinberg::Vst::kSample32,block,sampleRate};
    demand(processor->setupProcessing(setup)==Steinberg::kResultOk,"owned setupProcessing failed");
    auto arrangement=channels==1?Steinberg::Vst::SpeakerArr::kMono:Steinberg::Vst::SpeakerArr::kStereo;
    auto inputArrangement=arrangement,outputArrangement=arrangement;
    demand(processor->setBusArrangements(inputs?&inputArrangement:nullptr,inputs,&outputArrangement,1)==Steinberg::kResultOk,
           "owned playback rejected explicit bus layout; no implicit channel fallback");
    Steinberg::Vst::SpeakerArrangement actual=0;
    demand(processor->getBusArrangement(Steinberg::Vst::kOutput,0,actual)==Steinberg::kResultOk && actual==arrangement,
           "owned playback actual output arrangement differs from explicit request");
    if(inputs)demand(processor->getBusArrangement(Steinberg::Vst::kInput,0,actual)==Steinberg::kResultOk && actual==arrangement,
                     "owned playback actual input arrangement differs from explicit request");
    p->inputBusCount=inputs;
    if(inputs){p->inputBusEnabled=true;demand(p->component->activateBus(Steinberg::Vst::kAudio,Steinberg::Vst::kInput,0,true)==Steinberg::kResultOk,"owned input bus activation failed");}
    p->outputBusEnabled=true;demand(p->component->activateBus(Steinberg::Vst::kAudio,Steinberg::Vst::kOutput,0,true)==Steinberg::kResultOk,"owned output bus activation failed");
    p->activationEver=true;
    const auto active=p->component->setActive(true);if(active!=Steinberg::kResultOk){p->uncertain=true;throw std::runtime_error("owned setActive(true) failed");}p->active=true;
    const auto processing=processor->setProcessing(true);if(processing!=Steinberg::kResultOk){p->uncertain=true;throw std::runtime_error("owned setProcessing(true) failed");}p->processing=true;
    p->channels=channels;p->maximumBlock=block;
}
void OwnedVST3PlaybackCompanion::renderOwnedPlaybackBlock(std::int64_t position,int count,float* const output[]) {
    OwnedCall scope;p->checkThread();demand(p->active && p->processing && !p->uncertain && position>=0 && count>0 && count<=p->maximumBlock && output,
                                        "owned playback render called outside actual active processing");
    for(int channel=0;channel<p->channels;++channel){demand(output[channel]!=nullptr,"owned output buffer missing");std::memset(output[channel],0,(std::size_t)count*sizeof(float));}
    const std::array<float*,2> expectedBuffers {output[0],p->channels==2?output[1]:nullptr};
    Steinberg::Vst::AudioBusBuffers buffers{};buffers.numChannels=p->channels;buffers.channelBuffers32=const_cast<float**>(output);buffers.silenceFlags=0;
    Steinberg::Vst::AudioBusBuffers inputs=buffers;inputs.silenceFlags=~Steinberg::uint64(0);
    Steinberg::Vst::ProcessContext context{};context.state=Steinberg::Vst::ProcessContext::kPlaying;context.sampleRate=48000;context.projectTimeSamples=position;
    Steinberg::Vst::ProcessData data{};data.processMode=p->processMode;data.symbolicSampleSize=Steinberg::Vst::kSample32;data.numSamples=count;
    data.numInputs=p->inputBusCount;data.inputs=data.numInputs?&inputs:nullptr;
    data.numOutputs=1;data.outputs=&buffers;data.processContext=&context;
    demand(p->processor->process(data)==Steinberg::kResultOk,"actual owned VST3 ARA process failed");
    demand(buffers.numChannels==p->channels && buffers.channelBuffers32==output,"owned processor changed host channel buffer layout");
    for(int channel=0;channel<p->channels;++channel)demand(output[channel]==expectedBuffers[(std::size_t)channel],"owned processor replaced a host channel buffer pointer");
}
std::uint32_t OwnedVST3PlaybackCompanion::actualLatencySamples()const {OwnedCall scope;p->checkThread();demand(p->processor && !p->disposed,"owned latency queried after disposal");return p->processor->getLatencySamples();}
void OwnedVST3PlaybackCompanion::stopOwnedPlayback() {
    OwnedCall scope;p->checkThread();
    if(p->processing || p->uncertain){demand(p->processor && p->processor->setProcessing(false)==Steinberg::kResultOk,"owned setProcessing(false) failed");p->processing=false;}
    if(p->active || p->uncertain){demand(p->component && p->component->setActive(false)==Steinberg::kResultOk,"owned setActive(false) failed");p->active=false;}
    if(p->inputBusEnabled){demand(p->component->activateBus(Steinberg::Vst::kAudio,Steinberg::Vst::kInput,0,false)==Steinberg::kResultOk,"owned input bus deactivation failed");p->inputBusEnabled=false;}
    if(p->outputBusEnabled){demand(p->component->activateBus(Steinberg::Vst::kAudio,Steinberg::Vst::kOutput,0,false)==Steinberg::kResultOk,"owned output bus deactivation failed");p->outputBusEnabled=false;}
    p->uncertain=false;
}
void OwnedVST3PlaybackCompanion::disposeOwnedPlayback() {
    OwnedCall scope;p->checkThread();if(p->disposed)return;stopOwnedPlayback();demand(inactive(),"owned child not inactive for disposal");
    p->entry.reset(); // balances direct entry refs before this OWN original termination
    if(p->initialized){demand(p->component->terminate()==Steinberg::kResultOk,"owned original terminate failed");p->initialized=false;}
    if(p->processor){p->processor->release();p->processor=nullptr;}
    if(p->component){p->component->release();p->component=nullptr;}
    p->disposed=true;
}
} // namespace rysup::ara_lab
