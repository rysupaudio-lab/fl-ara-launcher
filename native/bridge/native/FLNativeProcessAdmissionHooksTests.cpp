// Offline ABI/VM-shadow test only: no DAW, plug-in, factory, audio or SDK host.
// Includes the implementation to test its private public-vtable publication.
#include "FLVST3NativeHooks.cpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <condition_variable>
namespace rysup::ara_lab {
thread_local bool fakeOwnedCompanionBypass=false;
bool ownedPlaybackCompanionCallOnThisThread()noexcept{return fakeOwnedCompanionBypass;}
}
using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace rysup::fl_ara::native_hooks;

struct FakeHandler final:IComponentHandler {
    int edits=0,releases=0;ParamID lastID=0;ParamValue lastValue=0;tresult result=kResultOk;
    tresult PLUGIN_API queryInterface(const TUID,void** out)override{if(out)*out=this;return kResultOk;}
    uint32 PLUGIN_API addRef()override{return 2;}
    uint32 PLUGIN_API release()override{++releases;return 1;}
    tresult PLUGIN_API beginEdit(ParamID)override{return kResultOk;}
    tresult PLUGIN_API performEdit(ParamID id,ParamValue value)override{++edits;lastID=id;lastValue=value;return result;}
    tresult PLUGIN_API endEdit(ParamID)override{return kResultOk;}
    tresult PLUGIN_API restartComponent(int32)override{return kResultOk;}
    virtual int privateTail(){return 12345;}
};
struct FakeController final:IEditController {
    int sets=0,params=0;IComponentHandler* actualHandler=nullptr;tresult result=kResultOk;
    ParamID lastID=0;ParamValue lastValue=0;bool synchronousEdit=false;
    tresult PLUGIN_API queryInterface(const TUID,void** out)override{if(out)*out=this;return kResultOk;}
    uint32 PLUGIN_API addRef()override{return 2;}
    uint32 PLUGIN_API release()override{return 1;}
    tresult PLUGIN_API initialize(FUnknown*)override{return kResultOk;}
    tresult PLUGIN_API terminate()override{return kResultOk;}
    tresult PLUGIN_API setComponentState(IBStream*)override{return kResultOk;}
    tresult PLUGIN_API setState(IBStream*)override{return kResultOk;}
    tresult PLUGIN_API getState(IBStream*)override{return kResultOk;}
    int32 PLUGIN_API getParameterCount()override{return 0;}
    tresult PLUGIN_API getParameterInfo(int32,ParameterInfo&)override{return kResultFalse;}
    tresult PLUGIN_API getParamStringByValue(ParamID,ParamValue,String128)override{return kResultFalse;}
    tresult PLUGIN_API getParamValueByString(ParamID,TChar*,ParamValue&)override{return kResultFalse;}
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID,ParamValue v)override{return v;}
    ParamValue PLUGIN_API plainParamToNormalized(ParamID,ParamValue v)override{return v;}
    ParamValue PLUGIN_API getParamNormalized(ParamID)override{return lastValue;}
    tresult PLUGIN_API setParamNormalized(ParamID id,ParamValue value)override{++params;lastID=id;lastValue=value;return result;}
    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler)override{
        ++sets;if(handler&&synchronousEdit)handler->performEdit(31,.25);if(result==kResultOk)actualHandler=handler;return result;
    }
    IPlugView* PLUGIN_API createView(FIDString)override{return nullptr;}
};
struct FakeProcessor final:IAudioProcessor {
    int processes=0;bool applyGain=false;ProcessData* lastData=nullptr;
    void(*processControl)(void*)=nullptr;void* controlContext=nullptr;
    tresult PLUGIN_API queryInterface(const TUID,void** out)override{if(out)*out=this;return kResultOk;}
    uint32 PLUGIN_API addRef()override{return 2;}
    uint32 PLUGIN_API release()override{return 1;}
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*,int32,SpeakerArrangement*,int32)override{return kResultOk;}
    tresult PLUGIN_API getBusArrangement(BusDirection,int32,SpeakerArrangement&)override{return kResultOk;}
    tresult PLUGIN_API canProcessSampleSize(int32)override{return kResultOk;}
    uint32 PLUGIN_API getLatencySamples()override{return 0;}
    tresult PLUGIN_API setupProcessing(ProcessSetup&)override{return kResultOk;}
    tresult PLUGIN_API setProcessing(TBool)override{return kResultOk;}
    tresult PLUGIN_API process(ProcessData& data)override{
        ++processes;lastData=&data;if(processControl)processControl(controlContext);
        if(applyGain&&data.outputs)for(int32 b=0;b<data.numOutputs;++b)for(int32 c=0;c<data.outputs[b].numChannels;++c)
            for(int32 s=0;s<data.numSamples;++s){
                if(data.symbolicSampleSize==kSample32)data.outputs[b].channelBuffers32[c][s]*=2;
                else if(data.symbolicSampleSize==kSample64)data.outputs[b].channelBuffers64[c][s]*=2;
            }
        return kResultFalse;
    }
    uint32 PLUGIN_API getTailSamples()override{return 0;}
};
struct FakeParameterChanges final:IParameterChanges {
    int32 count=0;int queries=0,otherSDKCalls=0;
    tresult PLUGIN_API queryInterface(const TUID,void** out)override{++otherSDKCalls;if(out)*out=this;return kResultOk;}
    uint32 PLUGIN_API addRef()override{++otherSDKCalls;return 2;}
    uint32 PLUGIN_API release()override{++otherSDKCalls;return 1;}
    int32 PLUGIN_API getParameterCount()override{++queries;return count;}
    IParamValueQueue* PLUGIN_API getParameterData(int32)override{++otherSDKCalls;return nullptr;}
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&,int32&)override{++otherSDKCalls;return nullptr;}
};
struct Observations {
    FakeHandler* handler=nullptr;FakeController* controller=nullptr;
    InstanceRecord* first=nullptr;int edits=0,params=0,before=0,after=0;
    FakeProcessor* processor=nullptr;const ProcessData* exactData=nullptr;
    InstanceRecord* processInstance=nullptr;
    FakeParameterChanges* queue=nullptr;int inputChanges=0,inputChangesAtBefore=0;
    int telemetryBefore=0,telemetryAfter=0,nativeCallsBefore=0;
    std::array<double,8> beforeSamples{};std::size_t captured=0;
};
// Distinct noninline helpers force actual SDK virtual dispatch after publication.
__attribute__((noinline)) tresult setHandler(IEditController* c,IComponentHandler* h){return c->setComponentHandler(h);}
__attribute__((noinline)) tresult setParam(IEditController* c,ParamID id,ParamValue v){return c->setParamNormalized(id,v);}
__attribute__((noinline)) tresult edit(IComponentHandler* h,ParamID id,ParamValue v){return h->performEdit(id,v);}
__attribute__((noinline)) tresult process(IAudioProcessor* p,ProcessData& d){return p->process(d);}
__attribute__((noinline)) int privateTail(FakeHandler* h){return h->privateTail();}

int main(){
    Runtime runtime;FakeHandler handler;FakeController c1,c2;FakeProcessor processor;
    InstanceRecord i1,i2,ip;Observations o;o.handler=&handler;o.controller=&c1;o.first=&i1;
    runtime.config.callbackContext=&o;
    o.processor=&processor;o.processInstance=&ip;
    runtime.config.callbacks.paramEditObserved=[](InstanceRecord* first,IComponentHandler* actual,ParamID id,ParamValue value,const ThreadInfo& thread,void* context)noexcept{
        auto& x=*static_cast<Observations*>(context);assert(first==x.first&&actual==x.handler);
        assert(x.handler->edits>x.edits&&id==x.handler->lastID&&value==x.handler->lastValue);
        assert(thread.hookDepth>0);++x.edits;
    };
    runtime.config.callbacks.paramNormalizedObserved=[](InstanceRecord&,IEditController* actual,ParamID id,ParamValue value,const ThreadInfo&,void* context)noexcept{
        auto& x=*static_cast<Observations*>(context);assert(actual==x.controller);
        assert(x.controller->params>x.params&&id==x.controller->lastID&&value==x.controller->lastValue);++x.params;
    };
    runtime.config.callbacks.beforeProcess=[](InstanceRecord& i,const ProcessData&,const ThreadInfo& t,void* context)noexcept{
        assert(t.insideNativeProcess&&i.nativeProcessAdmission.activeCount()==1);auto& x=*static_cast<Observations*>(context);
        assert(x.inputChanges==x.inputChangesAtBefore);++x.before;
    };
    runtime.config.callbacks.afterProcess=[](InstanceRecord&,const ProcessData&,tresult result,const ThreadInfo&,void* context)noexcept{
        assert(result==kResultFalse);++static_cast<Observations*>(context)->after;
    };
    runtime.config.callbacks.inputParameterChangesObserved=[](InstanceRecord&,IParameterChanges* queue,int32 count,const ThreadInfo& thread,void* context)noexcept{
        auto& x=*static_cast<Observations*>(context);assert(queue==x.queue&&count==x.queue->count&&count>0&&thread.insideNativeProcess);++x.inputChanges;
    };
    runtime.config.callbacks.nativeProcessTelemetry=[](InstanceRecord& i,const NativeProcessTelemetry& telemetry,const ThreadInfo& thread,void* context)noexcept{
        auto& x=*static_cast<Observations*>(context);
        assert(telemetry.actualNativeProcessor==x.processor&&telemetry.actualProcessData==x.exactData);
        assert(thread.insideNativeProcess&&thread.nativeThreadToken==reinterpret_cast<std::uintptr_t>(pthread_self()));
        assert(&i==x.processInstance&&i.nativeProcessAdmission.activeCount()==1);
        const auto& data=*telemetry.actualProcessData;
        if(telemetry.phase==NativeProcessTelemetryPhase::BeforeOriginal){
            assert(!telemetry.originalInvoked&&telemetry.originalResult==kInternalError);
            ++x.telemetryBefore;x.nativeCallsBefore=x.processor->processes;x.captured=0;
            if(x.processor->applyGain)for(int32 b=0;b<data.numOutputs;++b)for(int32 c=0;c<data.outputs[b].numChannels;++c)
                for(int32 s=0;s<data.numSamples;++s){assert(x.captured<x.beforeSamples.size());
                    x.beforeSamples[x.captured++]=data.symbolicSampleSize==kSample32?data.outputs[b].channelBuffers32[c][s]:data.outputs[b].channelBuffers64[c][s];}
        }else{
            assert(telemetry.originalInvoked&&telemetry.originalResult==kResultFalse&&x.processor->processes==x.nativeCallsBefore+1&&x.processor->lastData==x.exactData);++x.telemetryAfter;
            std::size_t index=0;
            if(x.processor->applyGain)for(int32 b=0;b<data.numOutputs;++b)for(int32 c=0;c<data.outputs[b].numChannels;++c)
                for(int32 s=0;s<data.numSamples;++s){const auto actual=data.symbolicSampleSize==kSample32?data.outputs[b].channelBuffers32[c][s]:data.outputs[b].channelBuffers64[c][s];
                    assert(index<x.captured&&actual==x.beforeSamples[index++]*2);}
        }
    };
    auto* s1=publishInterface(&runtime,&c1,Kind::Controller,nullptr,&i1);
    auto* s2=publishInterface(&runtime,&c2,Kind::Controller,nullptr,&i2);
    auto* sp=publishInterface(&runtime,&processor,Kind::Processor,nullptr,&ip);
    assert(s1&&s2&&sp);
    c1.synchronousEdit=true;
    assert(setHandler(&c1,&handler)==kResultOk&&c1.sets==1&&o.edits==1);
    assert(i1.actualNativeComponentHandler==&handler);
    auto* hs=slotFor(&handler);assert(hs&&hs->kind==Kind::Handler&&!hs->instance&&!hs->factory);
    assert(hs->publishedTable[7]==hs->originalTable[7]); // private virtual tail untouched
    assert(privateTail(&handler)==12345);
    assert(setHandler(&c2,&handler)==kResultOk&&c2.sets==1&&hs->handlerSharedAcrossControllers);
    i1.nativeTerminated=true;i1.nativeAlive=false;
    assert(edit(&handler,52,.75)==kResultOk&&o.edits==2&&handler.edits==2);
    handler.result=kResultFalse;assert(edit(&handler,53,.5)==kResultFalse&&o.edits==2&&handler.edits==3);
    handler.result=kResultOk;
    std::thread background([&]{assert(edit(&handler,54,.125)==kResultOk);});background.join();
    assert(o.edits==3&&handler.edits==4);
    i1.nativeAlive=true;i1.nativeTerminated=false;
    assert(setParam(&c1,71,.625)==kResultOk&&o.params==1&&c1.params==1);
    c1.result=kResultFalse;assert(setParam(&c1,72,.375)==kResultFalse&&o.params==1&&c1.params==2);
    c1.result=kResultOk;
    rysup::ara_lab::fakeOwnedCompanionBypass=true;
    assert(edit(&handler,55,.875)==kResultOk&&o.edits==3&&handler.edits==5);
    assert(setParam(&c1,73,.875)==kResultOk&&o.params==1&&c1.params==3);
    rysup::ara_lab::fakeOwnedCompanionBypass=false;
    // Genuine process hook with RAM buffers only; no renderer/device/host.
    float left[4]={1,2,3,4},right[4]={5,6,7,8};float* outputs32[]={left,right};
    AudioBusBuffers bus{};bus.numChannels=2;bus.channelBuffers32=outputs32;
    ProcessData data{};data.symbolicSampleSize=kSample32;data.numSamples=4;data.numOutputs=1;data.outputs=&bus;
    o.exactData=&data;
    assert(process(&processor,data)==kResultFalse&&processor.processes==1&&o.before==1&&o.after==1);
    ip.nativeProcessAdmission.setAllowed(false);
    assert(process(&processor,data)==kResultOk&&processor.processes==1&&o.before==1&&o.after==1);
    assert(left[0]==0&&left[3]==0&&right[0]==0&&right[3]==0&&bus.silenceFlags==3);
    double mono[4]={1,2,3,4};double* outputs64[]={mono};bus.numChannels=1;bus.channelBuffers64=outputs64;
    data.symbolicSampleSize=kSample64;
    assert(process(&processor,data)==kResultOk&&mono[0]==0&&mono[3]==0&&bus.silenceFlags==1);
    assert(ip.nativeProcessAdmission.activeCount()==0&&ip.blockedProcessCallbacks==2);
    rysup::ara_lab::fakeOwnedCompanionBypass=true;
    assert(process(&processor,data)==kResultFalse&&processor.processes==2&&o.before==1&&o.after==1);
    rysup::ara_lab::fakeOwnedCompanionBypass=false;
    assert(o.telemetryBefore==1&&o.telemetryAfter==1);
    // Empty/nonempty actual host queues: exactly one standard count getter,
    // positive notification before owner callback; no parameter/point queries.
    FakeParameterChanges queue;o.queue=&queue;data.inputParameterChanges=&queue;ip.nativeProcessAdmission.setAllowed(true);
    processor.applyGain=true;mono[0]=.25;mono[1]=.5;mono[2]=.75;mono[3]=1;
    assert(process(&processor,data)==kResultFalse&&queue.queries==1&&o.inputChanges==0&&mono[3]==2);
    queue.count=2;o.inputChangesAtBefore=1;
    assert(process(&processor,data)==kResultFalse&&queue.queries==2&&o.inputChanges==1&&mono[3]==4);
    assert(queue.otherSDKCalls==0&&o.telemetryBefore==3&&o.telemetryAfter==3&&processor.processes==4);
    ip.nativeProcessAdmission.setAllowed(false);
    assert(process(&processor,data)==kResultOk&&queue.queries==2&&o.inputChanges==1&&o.telemetryBefore==3&&o.telemetryAfter==3&&processor.processes==4);
    rysup::ara_lab::fakeOwnedCompanionBypass=true;
    assert(process(&processor,data)==kResultFalse&&queue.queries==2&&o.inputChanges==1&&o.telemetryBefore==3&&o.telemetryAfter==3&&processor.processes==5);
    rysup::ara_lab::fakeOwnedCompanionBypass=false;
    ip.nativeProcessAdmission.setAllowed(true);runtime.config.callbacks.inputParameterChangesObserved=nullptr;
    assert(process(&processor,data)==kResultFalse&&queue.queries==2&&o.telemetryBefore==4&&o.telemetryAfter==4&&processor.processes==6);
    runtime.callbacksEnabled=false;
    assert(process(&processor,data)==kResultFalse&&queue.queries==2&&o.telemetryBefore==4&&o.telemetryAfter==4&&processor.processes==7);
    assert(setHandler(&c1,nullptr)==kResultOk&&i1.actualNativeComponentHandler==nullptr&&c1.sets==2);
    // C8 actual hook control: close while the owner BEFORE callback is live,
    // then keep the owner AFTER callback live after the original returned.
    struct Held {
        std::mutex mutex;std::condition_variable condition;
        unsigned phase=0;bool beforeMayReturn=false,nativeMayReturn=false,afterMayReturn=false;
    } held;
    runtime.callbacksEnabled=true;runtime.config.callbackContext=&held;
    runtime.config.callbacks.nativeProcessTelemetry=nullptr;
    runtime.config.callbacks.beforeProcess=[](InstanceRecord& instance,const ProcessData&,const ThreadInfo&,void* context)noexcept{
        auto& h=*static_cast<Held*>(context);std::unique_lock<std::mutex> lock(h.mutex);
        assert(instance.nativeProcessAdmission.activeCount()==1);h.phase=1;h.condition.notify_all();
        h.condition.wait(lock,[&]{return h.beforeMayReturn;});
    };
    runtime.config.callbacks.afterProcess=[](InstanceRecord& instance,const ProcessData&,tresult result,const ThreadInfo&,void* context)noexcept{
        auto& h=*static_cast<Held*>(context);std::unique_lock<std::mutex> lock(h.mutex);
        assert(result==kResultFalse&&instance.nativeProcessAdmission.activeCount()==1);
        h.phase=3;h.condition.notify_all();h.condition.wait(lock,[&]{return h.afterMayReturn;});
    };
    processor.controlContext=&held;
    processor.processControl=[](void* context){auto& h=*static_cast<Held*>(context);
        std::unique_lock<std::mutex> lock(h.mutex);h.phase=2;h.condition.notify_all();
        h.condition.wait(lock,[&]{return h.nativeMayReturn;});};
    std::thread actualHook([&]{assert(process(&processor,data)==kResultFalse);});
    {std::unique_lock<std::mutex> lock(held.mutex);held.condition.wait(lock,[&]{return held.phase==1;});}
    assert(ip.nativeProcessAdmission.exchangeAllowed(false));
    assert(ip.nativeProcessAdmission.activeCount()==1&&processor.processes==7);
    {std::lock_guard<std::mutex> lock(held.mutex);held.beforeMayReturn=true;held.condition.notify_all();}
    {std::unique_lock<std::mutex> lock(held.mutex);held.condition.wait(lock,[&]{return held.phase==2;});}
    assert(ip.nativeProcessAdmission.activeCount()==1&&processor.processes==8);
    {std::lock_guard<std::mutex> lock(held.mutex);held.nativeMayReturn=true;held.condition.notify_all();}
    {std::unique_lock<std::mutex> lock(held.mutex);held.condition.wait(lock,[&]{return held.phase==3;});}
    assert(ip.nativeProcessAdmission.activeCount()==1&&processor.processes==8);
    double blockedSamples[2]={.5,.25};double* blockedChannels[]={blockedSamples};
    AudioBusBuffers blockedBus{};blockedBus.numChannels=1;blockedBus.channelBuffers64=blockedChannels;
    ProcessData blockedData{};blockedData.symbolicSampleSize=kSample64;blockedData.numSamples=2;blockedData.numOutputs=1;blockedData.outputs=&blockedBus;
    assert(process(&processor,blockedData)==kResultOk&&processor.processes==8);
    assert(ip.nativeProcessAdmission.activeCount()==1&&blockedSamples[0]==0&&blockedSamples[1]==0);
    {std::lock_guard<std::mutex> lock(held.mutex);held.afterMayReturn=true;held.condition.notify_all();}
    actualHook.join();assert(ip.nativeProcessAdmission.activeCount()==0&&!ip.nativeProcessAdmission.isAllowed());
    for(unsigned attempt=0;attempt<10000;++attempt){
        assert(process(&processor,blockedData)==kResultOk);
        const auto state=ip.nativeProcessAdmission.snapshot();assert(!state.allowed&&state.activeCount==0);
    }
    assert(processor.processes==8);ip.nativeProcessAdmission.shutdown();ip.nativeProcessAdmission.setAllowed(true);
    assert(!ip.nativeProcessAdmission.isAllowed()&&process(&processor,blockedData)==kResultOk&&processor.processes==8);
    std::puts("PASS C8 exact hook lease BEFORE/original/AFTER drain,10000 late closed callbacks,shutdown; fake SDK native edits/shared lifetime/TLS, exact-once process telemetry numeric gain and identity, empty/nonempty native queues/count-only getter, blocked/TLS observer suppression, private tail, float32/float64 RAM gate");
    // Published shadows intentionally last for this short fake process lifetime.
    return 0;
}
