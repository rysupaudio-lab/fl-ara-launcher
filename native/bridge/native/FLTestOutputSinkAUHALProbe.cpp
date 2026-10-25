// Explicit metadata-only probe. Intentionally contains no initialize/start/
// render/IOProc APIs. Only a new, owned, uninitialized AUHAL is configured.
#include "FLTestOutputSink.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace rysup::fl_ara::test_output;
namespace {
std::atomic<unsigned> actualCallbacks{0};
constexpr char uid[]="Pro Tools Audio Bridge 2-A_UID";
bool admitted(void*) noexcept{return true;}
bool exactDevice(AudioDeviceID device,void*) noexcept {
    AudioObjectPropertyAddress a{kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    CFStringRef value=nullptr;UInt32 size=sizeof(value);
    if(AudioObjectGetPropertyData(device,&a,0,nullptr,&size,&value)||size!=sizeof(value)||!value)return false;
    char text[2048]{};bool okay=CFStringGetCString(value,text,sizeof(text),kCFStringEncodingUTF8)&&!std::strcmp(text,uid);CFRelease(value);
    UInt32 transport=0;size=sizeof(transport);a.mSelector=kAudioDevicePropertyTransportType;
    return okay&&!AudioObjectGetPropertyData(device,&a,0,nullptr,&size,&transport)&&size==sizeof(transport)&&transport==kAudioDeviceTransportTypeVirtual;
}
bool running(AudioDeviceID device,UInt32& value) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyDeviceIsRunningSomewhere,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 size=sizeof(value);
    return !AudioObjectGetPropertyData(device,&a,0,nullptr,&size,&value)&&size==sizeof(value);
}
OSStatus probeCallback(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32,AudioBufferList*) {
    actualCallbacks.fetch_add(1,std::memory_order_relaxed);return kAudioUnitErr_CannotDoInCurrentContext;
}
struct Owner {AudioUnit unit=nullptr;OSStatus dispose(){if(!unit)return noErr;auto u=unit;unit=nullptr;return AudioComponentInstanceDispose(u);}~Owner(){dispose();}};
}
int main(int argc,char** argv){
    if((argc!=3&&argc!=4)||std::strcmp(argv[1],"--run-metadata-only")||(std::strcmp(argv[2],"1")&&std::strcmp(argv[2],"2"))||
       (argc==4&&std::strcmp(argv[3],"input")&&std::strcmp(argv[3],"output"))){std::cerr<<"usage: probe --run-metadata-only 1|2 [input|output]\n";return 2;}
    const UInt32 channels=!std::strcmp(argv[2],"1")?1:2;
    const AudioUnitScope callbackScope=(argc==4&&!std::strcmp(argv[3],"output"))?kAudioUnitScope_Output:kAudioUnitScope_Input;
    std::cout<<"{\"schema\":\"rysup.auhal-metadata-probe/1\",\"channels\":"<<channels<<",\"callbackScope\":"<<callbackScope;
    Owner owner;AudioDeviceID device=0;UInt32 before=0,last=0;
    const auto fail=[&](const char* stage,OSStatus status){auto disposed=owner.dispose();std::cout<<",\"result\":\"FAIL\",\"stage\":\""<<stage<<"\",\"status\":"<<status<<",\"disposeStatus\":"<<disposed<<",\"nativeCallbackCalls\":"<<actualCallbacks.load()<<",\"initializedOrStarted\":false}\n";return 1;};
    AudioObjectPropertyAddress devices{kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 size=0;
    auto status=AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&devices,0,nullptr,&size);if(status||size%sizeof(AudioDeviceID))return fail("enumerate-size",status);
    std::vector<AudioDeviceID> ids(size/sizeof(AudioDeviceID));status=AudioObjectGetPropertyData(kAudioObjectSystemObject,&devices,0,nullptr,&size,ids.data());if(status)return fail("enumerate",status);
    for(auto d:ids)if(exactDevice(d,nullptr)){if(device)return fail("ambiguous-device",-1);device=d;}
    if(!device)return fail("device-absent",-1);
    if(!running(device,before))return fail("read-running-before",-1);last=before;
    std::cout<<",\"exactVirtualUIDVerified\":true,\"deviceRunningBefore\":"<<before;
    const auto unchanged=[&](){UInt32 now=0;if(!running(device,now))return false;last=now;return now==before;};
    AudioComponentDescription description{};description.componentType=kAudioUnitType_Output;description.componentSubType=kAudioUnitSubType_HALOutput;description.componentManufacturer=kAudioUnitManufacturer_Apple;
    auto component=AudioComponentFindNext(nullptr,&description);if(!component)return fail("find-auhal",-1);
    status=AudioComponentInstanceNew(component,&owner.unit);if(status||!owner.unit)return fail("new-auhal",status);
    if(!unchanged())return fail("running-transition-after-new",-1);
    status=AudioUnitSetProperty(owner.unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,sizeof(device));if(status)return fail("set-current-device",status);
    if(!unchanged())return fail("running-transition-after-device",-1);
    AudioStreamBasicDescription format{48000,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved,4,1,4,channels,32,0};
    status=AudioUnitSetProperty(owner.unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format));if(status)return fail("set-format",status);
    if(!unchanged())return fail("running-transition-after-format",-1);
    Dl_info image{};if(!dladdr(reinterpret_cast<void*>(probeCallback),&image)||!image.dli_fname)return fail("probe-image",-1);
    FLTestOutputSink sink;Config config;config.exactAdmittedEnginePath=image.dli_fname;config.exactTestDeviceUID=uid;config.functions.currentlyAdmitted=admitted;config.functions.verifyExactTestDevice=exactDevice;
    if(!sink.initialize(config))return fail("sink-initialize",-1);
    AURenderCallbackStruct callback{probeCallback,&actualCallbacks};status=sink.observedSetProperty(owner.unit,kAudioUnitProperty_SetRenderCallback,callbackScope,0,&callback,sizeof(callback));if(status)return fail("set-wrapped-callback",status);
    UInt32 infoBytes=0;Boolean writable=false;auto infoStatus=AudioUnitGetPropertyInfo(owner.unit,kAudioUnitProperty_SetRenderCallback,callbackScope,0,&infoBytes,&writable);
    AURenderCallbackStruct readback{};size=sizeof(readback);auto callbackStatus=AudioUnitGetProperty(owner.unit,kAudioUnitProperty_SetRenderCallback,callbackScope,0,&readback,&size);
    const auto callbackBytes=size;
    std::cout<<",\"callbackInfoStatus\":"<<infoStatus<<",\"callbackInfoBytes\":"<<infoBytes<<",\"callbackWritable\":"<<(writable?"true":"false")<<",\"callbackGetStatus\":"<<callbackStatus<<",\"callbackGetBytes\":"<<size;
    AudioStreamBasicDescription current{};size=sizeof(current);status=AudioUnitGetProperty(owner.unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&current,&size);if(status||size!=sizeof(current))return fail("get-format",status);
    UInt32 maximum=0;size=sizeof(maximum);status=AudioUnitGetProperty(owner.unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,&size);if(status||size!=sizeof(maximum))return fail("get-maximum",status);
    std::cout<<",\"sampleRateReadback\":"<<current.mSampleRate<<",\"channelsReadback\":"<<current.mChannelsPerFrame<<",\"maximumFramesReadback\":"<<maximum;
    const bool writeOnly=callbackStatus==kAudioUnitErr_InvalidProperty&&callbackBytes==0&&infoStatus==noErr&&infoBytes==sizeof(AURenderCallbackStruct)&&writable;
    const bool outputAlias=callbackScope==kAudioUnitScope_Output&&callbackStatus==kAudioUnitErr_InvalidScope&&callbackBytes==sizeof(readback)&&infoStatus==kAudioUnitErr_InvalidScope&&infoBytes==0&&!writable;
    if(!writeOnly&&!outputAlias&&(callbackStatus||callbackBytes!=sizeof(readback)||!readback.inputProc||readback.inputProcRefCon==callback.inputProcRefCon))return fail("get-wrapped-callback",callbackStatus);
    if(!sink.tryPrepareMutedOutputStart(owner.unit))return fail("prepare-muted-start",-1);
    if(!unchanged())return fail("running-transition-after-prepare",-1);
    auto stats=sink.snapshot();if(actualCallbacks.load()||stats.callbackCalls)return fail("unexpected-callback",-1);
    auto disposed=owner.dispose();if(disposed)return fail("dispose",disposed);
    if(!unchanged())return fail("running-transition-after-dispose",-1);
    const auto diagnostic=stats.lastRegistration;
    if(!diagnostic.coherent||diagnostic.scope!=callbackScope||diagnostic.stage!=RegistrationStage::Registered||!diagnostic.originalInvoked||diagnostic.originalStatus)return fail("registration-diagnostic",-1);
    const auto metadata=stats.callbackMetadata;
    if(!metadata.coherent||!metadata.accepted||metadata.outputAlias!=outputAlias)return fail("metadata-diagnostic",-1);
    std::cout<<",\"result\":\"PASS\",\"callbackGetterAvailable\":"<<((writeOnly||outputAlias)?"false":"true")<<",\"outputAlias\":"<<(metadata.outputAlias?"true":"false")<<",\"inputGetterStatus\":"<<metadata.inputGetterStatus<<",\"inputGetterBytes\":"<<metadata.inputGetterBytes<<",\"inputInfoStatus\":"<<metadata.inputInfoStatus<<",\"inputInfoBytes\":"<<metadata.inputInfoBytes<<",\"inputWritable\":"<<(metadata.inputWritable?"true":"false")<<",\"writeOnlyCallbackPreparations\":"<<stats.writeOnlyCallbackPreparations<<",\"registrationStage\":"<<static_cast<unsigned>(diagnostic.stage)<<",\"registrationReason\":"<<static_cast<unsigned>(diagnostic.reason)<<",\"originalSetStatus\":"<<diagnostic.originalStatus<<",\"preparedButNeverStarted\":true,\"initializedOrStarted\":false,\"disposeStatus\":0,\"deviceRunningAfter\":"<<last<<",\"nativeCallbackCalls\":0,\"sinkCallbackCalls\":0}\n";
}
