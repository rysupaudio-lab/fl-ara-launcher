#include "FLTestOutputSink.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
static std::atomic<unsigned> allocations{0};
void* operator new(std::size_t n){allocations.fetch_add(1);if(void* p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
using namespace rysup::fl_ara::test_output;
struct List2 {UInt32 mNumberBuffers;AudioBuffer mBuffers[2];};
struct FakeUnit {
    AURenderCallbackStruct callback{};AudioStreamBasicDescription format{};
    UInt32 maximum=8192;AudioDeviceID device=42;bool output=true,writeOnly=false,setFails=false,writable=true;UInt32 infoBytes=sizeof(AURenderCallbackStruct);unsigned setCalls=0;
    AudioUnitScope setCallbackScope=99,getCallbackScope=99,infoScope=99;
    bool outputAlias=true,apple=true;OSStatus outputGetStatus=kAudioUnitErr_InvalidScope,outputInfoStatus=kAudioUnitErr_InvalidScope,inputGetStatus=kAudioUnitErr_InvalidProperty,inputInfoStatus=noErr;UInt32 outputGetBytes=16,outputInfoBytes=0,inputGetBytes=0;Boolean outputWritable=false;
};
static FakeUnit units[12];static bool admitted=true,deviceOkay=true;
static FLTestOutputSink* recursiveSink=nullptr;static RegistrationReason recursiveReason=RegistrationReason::None;
static std::array<unsigned,19> reasonCoverage{};
static FakeUnit& fake(AudioUnit u){auto n=reinterpret_cast<std::uintptr_t>(u)-0x1000;assert(n<12);return units[n];}
static OSStatus set(AudioUnit u,AudioUnitPropertyID p,AudioUnitScope scope,AudioUnitElement,const void* d,UInt32 size){auto& f=fake(u);++f.setCalls;if(recursiveSink){auto* sink=recursiveSink;recursiveSink=nullptr;assert(sink->observedSetProperty(u,p,scope,0,d,size)!=noErr);recursiveReason=sink->snapshot().lastRegistration.reason;}if(f.setFails)return kAudio_ParamError;if(p==kAudioUnitProperty_SetRenderCallback&&size==sizeof(f.callback)){std::memcpy(&f.callback,d,size);f.setCallbackScope=scope;}return noErr;}
static OSStatus get(AudioUnit u,AudioUnitPropertyID p,AudioUnitScope scope,AudioUnitElement,void* d,UInt32* size){auto& f=fake(u);if(p==kAudioUnitProperty_SetRenderCallback){f.getCallbackScope=scope;if(scope==kAudioUnitScope_Output&&f.outputAlias){*size=f.outputGetBytes;return f.outputGetStatus;}if(f.writeOnly){*size=f.inputGetBytes;return f.inputGetStatus;}assert(*size==sizeof(f.callback));std::memcpy(d,&f.callback,*size);}
    else if(p==kAudioOutputUnitProperty_CurrentDevice){assert(*size==sizeof(f.device));std::memcpy(d,&f.device,*size);}
    else if(p==kAudioUnitProperty_StreamFormat){assert(*size==sizeof(f.format));std::memcpy(d,&f.format,*size);}
    else if(p==kAudioUnitProperty_MaximumFramesPerSlice){assert(*size==sizeof(f.maximum));std::memcpy(d,&f.maximum,*size);}
    else return kAudio_ParamError;return noErr;}
static AudioComponent component(AudioComponentInstance u){return reinterpret_cast<AudioComponent>(u);}
static OSStatus description(AudioComponent c,AudioComponentDescription* d){auto& f=fake(reinterpret_cast<AudioUnit>(c));d->componentType=f.output?kAudioUnitType_Output:kAudioUnitType_Effect;d->componentManufacturer=f.apple?kAudioUnitManufacturer_Apple:0;return noErr;}
static OSStatus info(AudioUnit u,AudioUnitPropertyID,AudioUnitScope scope,AudioUnitElement,UInt32* size,Boolean* writable){auto& f=fake(u);f.infoScope=scope;if(scope==kAudioUnitScope_Output&&f.outputAlias){*size=f.outputInfoBytes;*writable=f.outputWritable;return f.outputInfoStatus;}*size=f.infoBytes;*writable=f.writable;return f.inputInfoStatus;}
static bool allowed(void*)noexcept{return admitted;}
static bool device(AudioDeviceID d,void*)noexcept{return deviceOkay&&d==42;}
struct Native {
    unsigned calls=0;bool alias=false,fail=false,malformed=false,reenter=false;
    AudioBufferList* hardware=nullptr;AURenderCallbackStruct outer{};
    AudioUnitRenderActionFlags* flags=nullptr;const AudioTimeStamp* time=nullptr;UInt32 bus=0,frames=0;
    std::array<float,16384> internal{};
    bool block=false;std::atomic<bool> entered{false},release{false};
};
static OSStatus native(void* ctx,AudioUnitRenderActionFlags* flags,const AudioTimeStamp* time,UInt32 bus,UInt32 frames,AudioBufferList* data){
    auto& n=*static_cast<Native*>(ctx);++n.calls;assert(flags==n.flags&&time==n.time&&bus==n.bus&&frames==n.frames&&data!=n.hardware);
    if(n.block){n.entered.store(true);while(!n.release.load())std::this_thread::yield();}
    for(UInt32 b=0;b<data->mNumberBuffers;++b){assert(data->mBuffers[b].mData!=n.hardware->mBuffers[b].mData);
        if(n.alias){n.internal.fill(0.75f);data->mBuffers[b].mData=n.internal.data();}
        else {auto* f=static_cast<float*>(data->mBuffers[b].mData);for(UInt32 i=0;i<data->mBuffers[b].mDataByteSize/sizeof(float);++i)f[i]=0.5f;}
    }
    if(n.reenter){n.reenter=false;auto status=n.outer.inputProc(n.outer.inputProcRefCon,flags,time,bus,frames,n.hardware);assert(status!=noErr);}
    if(n.malformed)data->mNumberBuffers=3;
    return n.fail?kAudio_ParamError:noErr;
}
static OSStatus otherNative(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32,AudioBufferList*){return noErr;}
struct Fixture {
    std::unique_ptr<FLTestOutputSink> sink{new FLTestOutputSink};Native n;
    std::array<float,32768> a{},b{};List2 hardware{};
    AudioTimeStamp time{};AudioUnitRenderActionFlags flags=0;
    AudioUnit unit=reinterpret_cast<AudioUnit>(std::uintptr_t{0x1000});
    Fixture(const char* engineOverride=nullptr,std::uint64_t callbackOffset=0){
        for(auto& u:units)u=FakeUnit{};admitted=deviceOkay=true;
        Dl_info image{};assert(dladdr(reinterpret_cast<void*>(native),&image));
        Config c;c.exactAdmittedEnginePath=engineOverride?engineOverride:image.dli_fname;c.exactTestDeviceUID="Pro Tools Audio Bridge 2-A_UID";
        c.functions={set,get,component,description,allowed,device,nullptr};c.functions.getPropertyInfo=info;c.expectedOutputCallbackOffset=callbackOffset;assert(sink->initialize(c));
        auto& f=fake(unit);f.format={48000,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved,4,1,4,2,32,0};
        hardware={2,{{1,16,a.data()},{1,16,b.data()}}};a.fill(0.9f);b.fill(0.8f);
        n.hardware=reinterpret_cast<AudioBufferList*>(&hardware);n.flags=&flags;n.time=&time;n.bus=0;n.frames=4;
    }
    bool registerCallback(AudioUnitScope scope=kAudioUnitScope_Input){AURenderCallbackStruct c{native,&n};return sink->observedSetProperty(unit,kAudioUnitProperty_SetRenderCallback,scope,0,&c,sizeof(c))==noErr;}
    ~Fixture(){++reasonCoverage[static_cast<unsigned>(sink->snapshot().lastRegistration.reason)];}
    bool prepare(){return sink->tryPrepareMutedOutputStart(unit);}
    OSStatus render(){n.outer=fake(unit).callback;return n.outer.inputProc(n.outer.inputProcRefCon,&flags,&time,0,n.frames,n.hardware);}
    void zeros(){for(UInt32 c=0;c<hardware.mNumberBuffers;++c){auto& buffer=hardware.mBuffers[c];for(UInt32 i=0;i<buffer.mDataByteSize;++i)assert(static_cast<unsigned char*>(buffer.mData)[i]==0);}}
};
int main(){unsigned checks=0;
    {Fixture f;assert(f.registerCallback()&&f.prepare());auto before=allocations.load();assert(f.render()==noErr);assert(allocations.load()==before);f.zeros();auto s=f.sink->snapshot();assert(f.n.calls==1&&s.nativeRenderCalls==1&&s.nativeRenderedFrames==4&&s.nonzeroScratchSamples==8&&s.mutedFrames==4&&(f.flags&kAudioUnitRenderAction_OutputIsSilence));++checks;}
    {Fixture f;f.n.alias=true;assert(f.registerCallback()&&f.prepare()&&f.render()==noErr);f.zeros();for(auto x:f.n.internal)assert(x==0.75f);assert(f.sink->snapshot().nonzeroScratchSamples==8);++checks;}
    {Fixture f;assert(f.registerCallback());assert(f.render()!=noErr);f.zeros();assert(f.n.calls==0);++checks;}
    {Fixture f;fake(f.unit).format.mSampleRate=44100;assert(f.registerCallback()&&!f.prepare());++checks;}
    {Fixture f;fake(f.unit).maximum=8193;assert(f.registerCallback()&&!f.prepare());++checks;}
    {Fixture f;deviceOkay=false;assert(f.registerCallback()&&!f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback());fake(f.unit).callback.inputProc=native;assert(!f.prepare());++checks;}
    {Fixture f;fake(f.unit).format.mFormatFlags|=kAudioFormatFlagIsBigEndian;assert(f.registerCallback()&&!f.prepare());++checks;}
    {Fixture f;fake(f.unit).format.mChannelsPerFrame=3;assert(f.registerCallback()&&!f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.frames=8193;f.hardware.mBuffers[0].mDataByteSize=f.hardware.mBuffers[1].mDataByteSize=8193*4;assert(f.render()!=noErr);f.zeros();assert(f.n.calls==0&&f.sink->snapshot().unsupportedCallbacks==1);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.fail=true;assert(f.render()!=noErr);f.zeros();assert(f.n.calls==1&&f.sink->snapshot().nativeErrors==1&&f.sink->snapshot().nativeRenderedFrames==0);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.malformed=true;assert(f.render()!=noErr);f.zeros();assert(f.sink->snapshot().returnedShapeFailures==1);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.reenter=true;assert(f.render()==noErr);f.zeros();assert(f.n.calls==1&&f.sink->snapshot().concurrentCallbacks==1);++checks;}
    {Fixture f;UInt32 v=1;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&v,sizeof(v))==noErr&&fake(f.unit).setCalls==1);++checks;}
    {Fixture f;fake(f.unit).output=false;assert(f.registerCallback());assert(reinterpret_cast<std::uintptr_t>(fake(f.unit).callback.inputProc)==reinterpret_cast<std::uintptr_t>(native)&&fake(f.unit).setCalls==1&&!f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());auto format=fake(f.unit).format;fake(f.unit).format.mChannelsPerFrame=1;fake(f.unit).format.mBytesPerFrame=4;assert(!f.prepare());fake(f.unit).format=format;assert(f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.frames=0;f.hardware.mBuffers[0].mDataByteSize=f.hardware.mBuffers[1].mDataByteSize=0;assert(f.render()==noErr&&f.n.calls==1);++checks;}
    {Fixture f;for(std::size_t i=0;i<FLTestOutputSink::maximumUnits;++i){f.unit=reinterpret_cast<AudioUnit>(0x1000+i);assert(f.registerCallback());}f.unit=reinterpret_cast<AudioUnit>(0x1008);assert(!f.registerCallback());++checks;}
    {Fixture f;auto& format=fake(f.unit).format;format.mChannelsPerFrame=1;f.hardware.mNumberBuffers=1;assert(f.registerCallback()&&f.prepare()&&f.render()==noErr);f.zeros();assert(f.sink->snapshot().nonzeroScratchSamples==4);++checks;}
    {Fixture f;auto& format=fake(f.unit).format;format.mFormatFlags&=~kAudioFormatFlagIsNonInterleaved;format.mBytesPerFrame=format.mBytesPerPacket=8;f.hardware.mNumberBuffers=1;f.hardware.mBuffers[0].mNumberChannels=2;f.hardware.mBuffers[0].mDataByteSize=32;assert(f.registerCallback()&&f.prepare()&&f.render()==noErr);f.zeros();assert(f.sink->snapshot().nonzeroScratchSamples==8);++checks;}
    {Fixture f;auto& format=fake(f.unit).format;format.mBitsPerChannel=64;format.mBytesPerFrame=format.mBytesPerPacket=8;f.hardware.mBuffers[0].mDataByteSize=f.hardware.mBuffers[1].mDataByteSize=32;assert(f.registerCallback()&&f.prepare()&&f.render()==noErr);f.zeros();assert(f.sink->snapshot().nonzeroScratchSamples==8);++checks;}
    {Fixture f;admitted=false;assert(!f.registerCallback()&&fake(f.unit).setCalls==0);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());AURenderCallbackStruct remove{};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&remove,sizeof(remove))==noErr&&!f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.frames=UINT32_MAX;assert(f.render()!=noErr);f.zeros();assert(f.n.calls==0);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.frames=8192;f.hardware.mBuffers[0].mDataByteSize=f.hardware.mBuffers[1].mDataByteSize=8192*4;auto before=allocations.load();assert(f.render()==noErr);assert(allocations.load()==before);f.zeros();assert(f.sink->snapshot().nonzeroScratchSamples==16384);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.block=true;f.n.outer=fake(f.unit).callback;std::thread first([&]{assert(f.n.outer.inputProc(f.n.outer.inputProcRefCon,&f.flags,&f.time,0,4,f.n.hardware)==noErr);});while(!f.n.entered.load())std::this_thread::yield();
        float x[4]{1,1,1,1},y[4]{1,1,1,1};List2 other{2,{{1,16,x},{1,16,y}}};AudioUnitRenderActionFlags flags=0;
        assert(f.n.outer.inputProc(f.n.outer.inputProcRefCon,&flags,&f.time,0,4,reinterpret_cast<AudioBufferList*>(&other))!=noErr);for(auto v:x)assert(v==0);for(auto v:y)assert(v==0);f.n.release.store(true);first.join();f.zeros();assert(f.n.calls==1&&f.sink->snapshot().concurrentCallbacks==1);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());fake(f.unit).device=99;assert(!f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());AURenderCallbackStruct changed{native,reinterpret_cast<void*>(0x1234)};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&changed,sizeof(changed))!=noErr);assert(f.render()!=noErr&&f.n.calls==0);f.zeros();++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;assert(f.registerCallback()&&f.prepare()&&f.render()==noErr);f.zeros();assert(f.sink->snapshot().writeOnlyCallbackPreparations==1);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).writable=false;assert(f.registerCallback()&&!f.prepare());++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).infoBytes=15;assert(f.registerCallback()&&!f.prepare());++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).setFails=true;assert(!f.registerCallback()&&!f.prepare());++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;assert(f.registerCallback()&&f.prepare());fake(f.unit).setFails=true;assert(!f.registerCallback()&&!f.prepare());assert(f.render()!=noErr&&f.n.calls==0);f.zeros();++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;assert(f.registerCallback()&&f.prepare());AURenderCallbackStruct c{native,&f.n};auto calls=fake(f.unit).setCalls;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Global,0,&c,sizeof(c))!=noErr&&!f.prepare()&&fake(f.unit).setCalls==calls);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());AURenderCallbackStruct c{native,&f.n};auto calls=fake(f.unit).setCalls;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c)-1)!=noErr&&!f.prepare()&&fake(f.unit).setCalls==calls);assert(f.render()!=noErr&&f.n.calls==0);f.zeros();++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());AURenderCallbackStruct c{reinterpret_cast<AURenderCallback>(AudioUnitRender),&f.n};auto calls=fake(f.unit).setCalls;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))!=noErr&&!f.prepare()&&fake(f.unit).setCalls==calls);assert(f.render()!=noErr&&f.n.calls==0);f.zeros();++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());auto format=fake(f.unit).format;auto calls=fake(f.unit).setCalls;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format))==noErr&&fake(f.unit).setCalls==calls+1);assert(f.render()!=noErr&&f.n.calls==0);f.zeros();assert(f.sink->snapshot().criticalPropertyDisarms==1&&f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());AudioDeviceID value=42;assert(f.sink->observedSetProperty(f.unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&value,sizeof(value))==noErr);assert(f.render()!=noErr&&f.n.calls==0);f.zeros();assert(f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());UInt32 maximum=8192;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum))==noErr);assert(f.render()!=noErr&&f.n.calls==0);f.zeros();assert(f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());f.n.block=true;f.n.outer=fake(f.unit).callback;std::thread first([&]{assert(f.n.outer.inputProc(f.n.outer.inputProcRefCon,&f.flags,&f.time,0,4,f.n.hardware)==noErr);});while(!f.n.entered.load())std::this_thread::yield();
        auto format=fake(f.unit).format;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format))==noErr);f.n.release.store(true);first.join();f.zeros();auto s=f.sink->snapshot();assert(f.n.calls==1&&s.nativeRenderedFrames==4&&s.mutedFrames==4&&s.criticalPropertyDisarms==1);assert(f.render()!=noErr&&f.n.calls==1);assert(f.prepare());++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;assert(f.registerCallback(kAudioUnitScope_Output)&&f.prepare()&&f.render()==noErr);f.zeros();auto d=f.sink->snapshot().lastRegistration;assert(d.coherent&&d.stage==RegistrationStage::Registered&&d.reason==RegistrationReason::None&&d.scope==2&&d.bytes==16&&d.callbackSource==CallbackSource::ExactEngine&&d.originalInvoked&&d.originalStatus==0&&!d.recordPresent&&d.claimHeld);assert(fake(f.unit).setCallbackScope==2&&fake(f.unit).getCallbackScope==1&&fake(f.unit).infoScope==1);++checks;}
    {Fixture f;AURenderCallbackStruct c{native,&f.n};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Global,0,&c,sizeof(c))!=noErr);assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::UnsupportedScope);++checks;}
    {Fixture f;AURenderCallbackStruct c{native,&f.n};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Output,1,&c,sizeof(c))!=noErr);assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::UnsupportedElement);++checks;}
    {Fixture f;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Output,0,nullptr,16)!=noErr);assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::MissingData);++checks;}
    {Fixture f;AURenderCallbackStruct c{native,&f.n};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Output,0,&c,15)!=noErr);auto d=f.sink->snapshot().lastRegistration;assert(d.reason==RegistrationReason::WrongDataBytes&&d.bytes==15&&!d.originalInvoked&&d.scope==2);++checks;}
    {Fixture f;admitted=false;assert(!f.registerCallback());assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::NotAdmitted);++checks;}
    {Fixture f;assert(f.registerCallback());auto c=fake(f.unit).callback;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))!=noErr);auto d=f.sink->snapshot().lastRegistration;assert(d.reason==RegistrationReason::SelfCallback&&d.callbackSource==CallbackSource::SelfWrapper&&d.recordPresent);++checks;}
    {Fixture f;AURenderCallbackStruct c{reinterpret_cast<AURenderCallback>(std::uintptr_t{1}),&f.n};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))!=noErr);auto d=f.sink->snapshot().lastRegistration;assert(d.reason==RegistrationReason::NoCallbackImage&&d.callbackSource==CallbackSource::NoImage);++checks;}
    {Fixture f("/bin/ls");assert(!f.registerCallback());auto d=f.sink->snapshot().lastRegistration;assert(d.reason==RegistrationReason::CallbackNotEngine&&d.callbackSource==CallbackSource::OwnedExecutable&&d.callbackImageOffset>0);++checks;}
    {Fixture f;assert(f.registerCallback());AURenderCallbackStruct c{otherNative,&f.n};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))!=noErr);assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::CallbackChanged);++checks;}
    {Fixture f;assert(f.registerCallback());AURenderCallbackStruct c{native,reinterpret_cast<void*>(1)};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))!=noErr);assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::RefconChanged);++checks;}
    {Fixture f;assert(f.registerCallback(kAudioUnitScope_Input));assert(!f.registerCallback(kAudioUnitScope_Output));assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::RegistrationRouteChanged);++checks;}
    {Fixture f;for(std::size_t i=0;i<FLTestOutputSink::maximumUnits;++i){f.unit=reinterpret_cast<AudioUnit>(0x1000+i);assert(f.registerCallback());}f.unit=reinterpret_cast<AudioUnit>(0x1008);assert(!f.registerCallback());assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::PoolExhausted);++checks;}
    {Fixture f;fake(f.unit).setFails=true;assert(!f.registerCallback());auto d=f.sink->snapshot().lastRegistration;assert(d.stage==RegistrationStage::OriginalFailed&&d.reason==RegistrationReason::OriginalSetterFailed&&d.originalInvoked&&d.originalStatus==kAudio_ParamError);++checks;}
    {auto sink=std::make_unique<FLTestOutputSink>();AURenderCallbackStruct c{native,nullptr};assert(sink->observedSetProperty(reinterpret_cast<AudioUnit>(0x1000),kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))!=noErr);assert(sink->snapshot().lastRegistration.reason==RegistrationReason::NotInitialized);++reasonCoverage[1];++checks;}
    {Fixture f;recursiveReason=RegistrationReason::None;recursiveSink=f.sink.get();assert(f.registerCallback());assert(recursiveReason==RegistrationReason::LifecycleBusy);++reasonCoverage[18];++checks;}
    {Fixture f;AURenderCallbackStruct c{reinterpret_cast<AURenderCallback>(AudioUnitRender),&f.n};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))!=noErr);auto d=f.sink->snapshot().lastRegistration;assert(d.callbackSource==CallbackSource::OtherImage&&(d.reason==RegistrationReason::CallbackPathUnresolved||d.reason==RegistrationReason::CallbackNotEngine));++checks;}
    {Fixture f;std::atomic<bool> done{false};std::thread reader([&]{while(!done.load()){auto d=f.sink->snapshot().lastRegistration;if(d.coherent&&d.sequence){assert(d.property==kAudioUnitProperty_SetRenderCallback&&d.scope==2&&d.element==0&&!d.originalInvoked);assert(d.bytes==14||d.bytes==15);assert(d.reason==RegistrationReason::WrongDataBytes||d.reason==RegistrationReason::ConfigurationBusy||d.reason==RegistrationReason::LifecycleBusy);}}});std::thread other([&]{AURenderCallbackStruct c{native,&f.n};for(unsigned i=0;i!=1000;++i)f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Output,0,&c,14);});AURenderCallbackStruct c{native,&f.n};for(unsigned i=0;i!=1000;++i)f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Output,0,&c,15);other.join();done=true;reader.join();assert(f.sink->snapshot().lastRegistration.coherent);++checks;}
    {Fixture f(nullptr,1);assert(!f.registerCallback(kAudioUnitScope_Output));auto d=f.sink->snapshot().lastRegistration;assert(d.reason==RegistrationReason::CallbackOffsetMismatch&&d.callbackSource==CallbackSource::ExactEngine&&!d.originalInvoked);++checks;}
    {Dl_info image{};assert(dladdr(reinterpret_cast<void*>(native),&image));auto offset=reinterpret_cast<std::uintptr_t>(native)-reinterpret_cast<std::uintptr_t>(image.dli_fbase);Fixture f(nullptr,offset);fake(f.unit).writeOnly=true;assert(f.registerCallback(kAudioUnitScope_Output)&&f.prepare());auto s=f.sink->snapshot();assert(s.lastRegistration.callbackImageOffset==offset&&s.callbackMetadata.coherent&&s.callbackMetadata.outputAlias&&s.callbackMetadata.accepted&&s.callbackMetadata.routeGetterStatus==kAudioUnitErr_InvalidScope&&s.callbackMetadata.routeGetterBytes==16&&s.callbackMetadata.routeInfoStatus==kAudioUnitErr_InvalidScope&&s.callbackMetadata.routeInfoBytes==0&&s.callbackMetadata.inputGetterStatus==kAudioUnitErr_InvalidProperty&&s.callbackMetadata.inputGetterBytes==0&&s.callbackMetadata.inputInfoStatus==0&&s.callbackMetadata.inputInfoBytes==16&&s.callbackMetadata.inputWritable);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).outputGetStatus=kAudio_ParamError;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.coherent&&d.routeGetterStatus==kAudio_ParamError&&!d.accepted&&!d.inputQueried);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).outputGetBytes=0;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.routeGetterBytes==0&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).outputInfoStatus=noErr;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.routeInfoStatus==0&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).outputInfoBytes=16;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.routeInfoBytes==16&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).outputWritable=true;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.routeWritable&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).inputGetStatus=kAudio_ParamError;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.inputQueried&&d.inputGetterStatus==kAudio_ParamError&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).inputGetBytes=16;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.inputGetterBytes==16&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).infoBytes=15;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.inputInfoBytes==15&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).writable=false;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(!d.inputWritable&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).apple=false;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());assert(!f.sink->snapshot().callbackMetadata.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;assert(f.registerCallback(kAudioUnitScope_Output));std::atomic<bool> done{false};std::thread reader([&]{while(!done.load()){auto d=f.sink->snapshot().callbackMetadata;if(d.coherent&&d.sequence){assert(d.registeredScope==2&&d.routeQueried&&d.inputQueried&&d.outputAlias&&d.accepted&&d.routeGetterStatus==kAudioUnitErr_InvalidScope&&d.routeGetterBytes==16&&d.inputGetterStatus==kAudioUnitErr_InvalidProperty&&d.inputGetterBytes==0&&d.inputInfoBytes==16);}}});for(unsigned i=0;i!=1000;++i)assert(f.prepare());done=true;reader.join();++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).inputInfoStatus=kAudio_ParamError;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.inputInfoStatus==kAudio_ParamError&&!d.accepted);++checks;}
    {Fixture f;fake(f.unit).writeOnly=true;fake(f.unit).outputGetStatus=noErr;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.routeGetterStatus==0&&!d.accepted);++checks;}
    {Fixture f;assert(f.registerCallback(kAudioUnitScope_Output)&&!f.prepare());auto d=f.sink->snapshot().callbackMetadata;assert(d.inputGetterStatus==0&&d.inputGetterBytes==16&&!d.accepted);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));auto lease=f.sink->tryAcquirePreparedStart(ticket);assert(lease&&lease.stillPrepared()&&ticket.device()==42&&ticket.generation()>0);assert(!f.sink->tryAcquirePreparedStart(ticket));assert(lease.stillPrepared());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));UInt32 maximum=8192;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum))==noErr);assert(!f.sink->tryAcquirePreparedStart(ticket)&&!f.sink->authorizePreparedTicket(ticket));assert(f.prepare());FLTestOutputSink::PreparedOutputTicket fresh;assert(f.sink->capturePreparedTicket(f.unit,fresh)&&fresh.generation()!=ticket.generation());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));auto lease=f.sink->tryAcquirePreparedStart(ticket);auto calls=fake(f.unit).setCalls;AURenderCallbackStruct c{native,reinterpret_cast<void*>(1)};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))!=noErr&&fake(f.unit).setCalls==calls&&lease.stillPrepared());assert(f.sink->snapshot().lastRegistration.reason==RegistrationReason::LifecycleBusy);assert(f.render()==noErr);f.zeros();++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));auto lease=f.sink->tryAcquirePreparedStart(ticket);auto calls=fake(f.unit).setCalls;auto format=fake(f.unit).format;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format))!=noErr&&fake(f.unit).setCalls==calls&&lease.stillPrepared());assert(!f.sink->tryAcquireLifecycleMutation(f.unit,true,true)&&lease.stillPrepared());assert(!f.prepare()&&lease.stillPrepared());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));auto lease=f.sink->tryAcquirePreparedStart(ticket);bool refused=false;std::thread other([&]{refused=!f.sink->tryAcquireLifecycleMutation(f.unit,true,true);});other.join();assert(refused&&lease.stillPrepared());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));{auto mutation=f.sink->tryAcquireLifecycleMutation(f.unit);assert(mutation&&!f.sink->tryAcquirePreparedStart(ticket));}assert(!f.sink->tryAcquirePreparedStart(ticket)&&f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));{auto lease=f.sink->tryAcquirePreparedStart(ticket);assert(lease);lease.invalidateAfterFailedStart();assert(!lease.stillPrepared());}assert(!f.sink->tryAcquirePreparedStart(ticket)&&f.prepare());++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));{auto disposal=f.sink->tryAcquireLifecycleMutation(f.unit,true,true);assert(disposal);}assert(!f.sink->authorizePreparedTicket(ticket));assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket fresh;assert(f.sink->capturePreparedTicket(f.unit,fresh)&&fresh.identity()!=ticket.identity());assert(!f.sink->tryAcquirePreparedStart(ticket));++checks;}
    {Fixture f;FLTestOutputSink::PreparedOutputTicket invalid;assert(!f.sink->tryAcquirePreparedStart(invalid));auto ordinary=f.sink->tryAcquireLifecycleMutation(f.unit,true,true);assert(ordinary);++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());FLTestOutputSink::PreparedOutputTicket ticket;assert(f.sink->capturePreparedTicket(f.unit,ticket));{auto lease=f.sink->tryAcquirePreparedStart(ticket);AURenderCallbackStruct removed{};auto calls=fake(f.unit).setCalls;assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&removed,sizeof(removed))!=noErr&&fake(f.unit).setCalls==calls&&lease.stillPrepared());}AURenderCallbackStruct removed{};assert(f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&removed,sizeof(removed))==noErr&&!f.sink->authorizePreparedTicket(ticket));++checks;}
    {Fixture f;auto untracked=f.sink->tryAcquireLifecycleMutation(f.unit);assert(untracked&&f.sink->unitMutationHeldByCurrentThread(f.unit));bool refused=false;std::thread other([&]{refused=!f.registerCallback();});other.join();assert(refused);FLTestOutputSink::PreparedOutputTicket ticket;assert(!f.sink->capturePreparedTicket(f.unit,ticket));++checks;}
    {Fixture f;{auto untracked=f.sink->tryAcquireLifecycleMutation(f.unit,true,true);assert(untracked&&!f.registerCallback());}assert(f.registerCallback()&&f.prepare());++checks;}
    {Fixture f;std::array<FLTestOutputSink::LifecycleLease,32> reservations;for(std::size_t i=0;i<reservations.size();++i){reservations[i]=f.sink->tryAcquireLifecycleMutation(reinterpret_cast<AudioUnit>(0x5000+i));assert(reservations[i]);}assert(!f.sink->tryAcquireLifecycleMutation(reinterpret_cast<AudioUnit>(0x9999)));++checks;}
    {Fixture f;assert(f.registerCallback()&&f.prepare());auto mutation=f.sink->tryAcquireLifecycleMutation(f.unit);assert(mutation&&f.sink->unitMutationHeldByCurrentThread(f.unit));bool foreign=false;std::thread other([&]{foreign=f.sink->unitMutationHeldByCurrentThread(f.unit);});other.join();assert(!foreign);++checks;}
    {for(unsigned attempt=0;attempt!=100;++attempt){Fixture f;std::atomic<bool> go{false};bool a=false,b=false;
        std::thread first([&]{while(!go.load())std::this_thread::yield();AURenderCallbackStruct c{native,reinterpret_cast<void*>(1)};a=f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))==noErr;});
        std::thread second([&]{while(!go.load())std::this_thread::yield();AURenderCallbackStruct c{native,reinterpret_cast<void*>(2)};b=f.sink->observedSetProperty(f.unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&c,sizeof(c))==noErr;});
        go=true;first.join();second.join();assert(!(a&&b)&&fake(f.unit).setCalls<=1);
    }++checks;}
    for(unsigned i=1;i!=19;++i){if(i==3)continue;if(!reasonCoverage[i])std::cerr<<"Uncovered refusal reason "<<i<<'\n';assert(reasonCoverage[i]);}++checks;
    std::cout<<"PASS "<<checks<<" muted-output controls; all registration/lifecycle controls covered; no AudioUnit/device created or started\n";
    return 0;
}
