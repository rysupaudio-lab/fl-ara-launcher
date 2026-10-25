// Process-local normal FL application admission. Original installed binaries,
// vendor signatures and native creation arguments are never changed.
#include "FLNativeApplication.h"
#include "RegistrationReadThrough.h"
#include "FLTestOutputSink.h"
#include "FLUnitDeviceGate.h"
#include "FLPointerProvenance.h"
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits.h>
#include <sstream>
#include <stdexcept>
#include <new>
#include <mutex>

namespace rysup::fl_ara { namespace {
constexpr const char* exeSHA="5be78aa9f9f066f3b28747b48357769b70979e859b42661f2af7163f08e57aab";
constexpr const char* engineSHA="8d23eebf0fc63fdd1957ba2c8d0f1f1d4e053a9f43173d3c22606c47ace7b76d";
constexpr const char* appleAudioComponent="/System/Library/Components/CoreAudio.component/Contents/MacOS/CoreAudio";
constexpr const char* appleAudioComponentSHA="64635af4212b6c5f257182cdbcbcdf553081a9e23f971fc4b9e0f4b1fb901f87";
using UnitGate=test_output::FLUnitDeviceGate;
constexpr const char* wrapperSHA="445dd1766e95013acc5046d7f08346fa04ef451a5d16e035a800518199c4d456";
struct State {
    NativeApplication app;NativeApplicationCallbacks callbacks;
    const bool testModeRequested=getenv("RYSUP_FLARA_TEST_AUDIO_DEVICE_UID")!=nullptr;
    CFRunLoopTimerRef timer=nullptr;int log=-1;void** actualOutArgument=nullptr;
    std::string testDeviceUID;std::atomic<bool> enabled{false};
    std::atomic<test_output::FLTestOutputSink*> testSink{nullptr};
    std::atomic<std::uint64_t> permittedDeviceStarts{0},refusedDeviceStarts{0};
    std::mutex outputStartDiagnosticMutex;
    NativeOutputStartDiagnostic outputStartDiagnostic;
    UnitGate unitDeviceGate;std::string appleComponentPath;std::atomic<bool> appleProfileAdmitted{false};
    std::array<std::atomic<std::uint64_t>,11> unitDeviceCounts{};
    std::mutex deviceIODiagnosticMutex;NativeAudioTestStats::DeviceIODiagnostic deviceIODiagnostic;
};
State& state(){static State value;return value;}
std::string canonical(const char* p){char value[PATH_MAX];return p&&realpath(p,value)?value:"";}
std::string hex(const void* bytes,std::size_t size){static const char* digits="0123456789abcdef";std::string s(size*2,'0');auto p=(const unsigned char*)bytes;for(std::size_t i=0;i<size;++i){s[i*2]=digits[p[i]>>4];s[i*2+1]=digits[p[i]&15];}return s;}
std::string shaFile(const std::string& path){int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return{};struct stat a{},b{};if(fstat(fd,&a)||!S_ISREG(a.st_mode)){close(fd);return{};}CC_SHA256_CTX hash;CC_SHA256_Init(&hash);unsigned char bytes[65536];ssize_t n=0;while((n=read(fd,bytes,sizeof(bytes)))>0)CC_SHA256_Update(&hash,bytes,(CC_LONG)n);const bool okay=n==0&&!fstat(fd,&b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size&&a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec&&a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec;close(fd);if(!okay)return{};unsigned char digest[32];CC_SHA256_Final(digest,&hash);return hex(digest,32);}
template<class T>bool readAt(std::uintptr_t p,T& out)noexcept{mach_vm_size_t count=0;return p&&mach_vm_read_overwrite(mach_task_self(),p,sizeof(T),(mach_vm_address_t)&out,&count)==KERN_SUCCESS&&count==sizeof(T);}
bool imageAt(std::uintptr_t p,const std::string& path,std::uintptr_t offset)noexcept{Dl_info image{};return p&&dladdr((void*)p,&image)&&canonical(image.dli_fname)==path&&p-(std::uintptr_t)image.dli_fbase==offset;}
bool objectAt(std::uintptr_t p,const std::string& path,std::uintptr_t vmt)noexcept{std::uintptr_t table=0;return readAt(p,table)&&imageAt(table,path,vmt);}
bool wrapperAssociationSealed(std::uintptr_t context)noexcept{
    const auto& path=state().app.wrapperPath;std::uintptr_t table=0;Dl_info image{};
    if(!readAt(context,table)||!imageAt(table,path,0x2b3dc8)||!dladdr((void*)table,&image))return false;
    const auto base=(std::uintptr_t)image.dli_fbase;
    if(!base||base>UINTPTR_MAX-0x8c398)return false;
    // The complete wrapper file was hash-admitted at startup. These exact
    // read-only ARM64 pins additionally bind the live field/initialization
    // contract; they never alter vendor instructions or call plugin methods.
    struct Pin{std::uintptr_t offset;std::uint32_t value;};
    constexpr Pin pins[]={
        {0x7f9f0,0xf94013e2u},
        {0x7f9f4,0xd2800021u},
        {0x7f9f8,0xb0001180u},
        {0x7f9fc,0x91004000u},
        {0x7fa00,0x94003244u},
        {0x7fa04,0xf94013e1u},
        {0x7fa08,0xf903d420u},
        {0x8c324,0xf90003e2u},
        {0x8c38c,0xf9400be1u},
        {0x8c390,0xf94003e0u},
        {0x8c394,0xf9000420u},
        {0x83884,0x91206043u},
        {0x83898,0xf9401844u},
        {0x838a0,0xd63f0080u},
        {0x83900,0xf943d401u},
        {0x8390c,0x91006021u},
        {0x83914,0xf9440c00u},
        {0x8391c,0xf9440c42u},
        {0x83924,0xf9400c42u},
        {0x83928,0xd63f0040u},
        {0x83968,0x9120a002u},
        {0x83970,0xf9440c00u},
        {0x83980,0xf9400063u},
        {0x83984,0xd63f0060u},
        {0x839cc,0xd63f0040u},
        {0x83a54,0x9120a023u},
        {0x83a6c,0xf9401884u},
        {0x83a70,0xd63f0080u},
        {0x83a8c,0xf943d401u},
        {0x83a98,0x91006021u},
        {0x83aa0,0xf9441400u},
        {0x83aa8,0xf9441442u},
        {0x83ab0,0xf9400c42u},
        {0x83ab4,0xd63f0040u},
        {0x88ad8,0xf9441400u},
        {0x88af0,0xf9441442u},
        {0x88af8,0xf9401442u},
        {0x88afc,0xd63f0040u},
    };
    for(const auto& pin:pins){std::uint32_t value=0;if(!readAt(base+pin.offset,value)||value!=pin.value)return false;}
    return true;
}
bool uuid(const std::string& s){if(s.size()!=36)return false;for(std::size_t i=0;i<s.size();++i){const auto c=s[i];if(i==8||i==13||i==18||i==23){if(c!='-')return false;}else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;}return true;}
std::string quoted(const std::string& s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c>=32)out+=char(c);}return out+'"';}
bool refreshEngine()noexcept{
    auto& s=state();if(!pthread_main_np())return false;const bool previouslyReady=s.app.engineReady;s.app.engineReady=false;
    if(!s.enabled.load()||!s.app.creationEntered||s.app.creationReturned||!s.actualOutArgument)return false;
    std::uintptr_t instance=0;if(!readAt((std::uintptr_t)s.actualOutArgument,instance)||!instance)return false;
    Dl_info image{};std::uintptr_t vmt=0;if(!readAt(instance,vmt)||!dladdr((void*)vmt,&image)||canonical(image.dli_fname)!=s.app.enginePath||vmt-(std::uintptr_t)image.dli_fbase!=0x146b6a8)return false;
    const auto base=(std::uintptr_t)image.dli_fbase;std::uintptr_t nativeInstance=0,nativeHost=0;
    if(!readAt(base+0x1b36338,nativeInstance)||nativeInstance!=instance||!readAt(base+0x1b36330,nativeHost)||nativeHost!=s.app.startupHost)return false;
    s.app.engine.imageBase=base;s.app.engine.exactLoadedImagePath=s.app.enginePath;s.app.engine.verifiedArm64SliceSHA256=rysup::ara_lab::kFLNativeModelArm64SHA256;
    const bool changed=s.app.engineInstance!=instance;s.app.engineInstance=instance;s.app.engineReady=true;
    if(!previouslyReady||changed)writeNativeApplicationEvent("actual_normal_fl_engine_ready");
    return true;
}
void timerTick(CFRunLoopTimerRef,void*)noexcept{auto& s=state();if(refreshEngine()&&s.callbacks.tick)s.callbacks.tick(s.app,s.callbacks.context);}
using Create=int(*)(void*,void**);
Create originalCreate=nullptr;
int nativeCreate(void* host,void** out){auto& s=state();const auto fn=originalCreate;if(!fn)return -1;
    if(!s.enabled.load())return fn(host,out);
    // An unsupported off-main entry never reaches the original constructor.
    // It cannot use startup exit: main may already own an active constructor.
    if(!pthread_main_np())return -1;
    // A second constructor is a runtime refusal, never a startup-only exit:
    // the first constructor can still own an open project and unsaved work.
    if(s.app.creationEntered){writeNativeApplicationEvent("owned_native_create_reentry_refused");return -1;}
    if(!host||!out){
        writeNativeApplicationEvent("owned_startup_create_contract_refused");_exit(78);
    }
    s.app.creationEntered=true;s.app.startupHost=(std::uintptr_t)host;s.actualOutArgument=out;
    Dl_info loaded{};
    const auto address=reinterpret_cast<std::uintptr_t>(fn);
    if(!dladdr(reinterpret_cast<void*>(address),&loaded)||canonical(loaded.dli_fname)!=s.app.enginePath
        ||(address-reinterpret_cast<std::uintptr_t>(loaded.dli_fbase)!=0xa410
           &&address-reinterpret_cast<std::uintptr_t>(loaded.dli_fbase)!=0x9ef0)){
        writeNativeApplicationEvent("owned_startup_engine_identity_refused");_exit(78);
    }
    s.app.engine.imageBase=reinterpret_cast<std::uintptr_t>(loaded.dli_fbase);
    s.app.engine.exactLoadedImagePath=s.app.enginePath;
    s.app.engine.verifiedArm64SliceSHA256=rysup::ara_lab::kFLNativeModelArm64SHA256;
    if(s.callbacks.beforeOriginalCreate&&!s.callbacks.beforeOriginalCreate(s.app,s.callbacks.context)){
        // The native constructor has not run and no project/session is open.
        // Never continue unbridged into a saved ARA project after guard failure.
        writeNativeApplicationEvent("owned_startup_save_guard_refused");_exit(78);
    }
    CFRunLoopTimerContext context{0,nullptr,nullptr,nullptr,nullptr};s.timer=CFRunLoopTimerCreate(kCFAllocatorDefault,CFAbsoluteTimeGetCurrent()+0.10,0.10,0,0,timerTick,&context);
    if(s.timer)CFRunLoopAddTimer(CFRunLoopGetMain(),s.timer,kCFRunLoopCommonModes);
    writeNativeApplicationEvent("actual_normal_fl_create_entered");const int result=fn(host,out);
    s.app.creationReturned=true;s.app.engineReady=false;s.actualOutArgument=nullptr;
    if(s.timer){CFRunLoopTimerInvalidate(s.timer);CFRelease(s.timer);s.timer=nullptr;}
    writeNativeApplicationEvent("actual_normal_fl_create_returned",std::to_string(result));
    if(s.callbacks.creationReturned)s.callbacks.creationReturned(s.app,result,s.callbacks.context);return result;
}
void* observedDlsym(void* handle,const char* name){void* result=dlsym(handle,name);auto& s=state();
    if(!result||!name||!s.enabled.load())return result;
    // Route only real symbols from the exact sealed, already native-loaded
    // WinBridge. Ordinary registry traffic stays on the private hive. This
    // never changes the vendor registration validator or writes license data.
    result=observeRegistrationSymbol(name,result);
    if(std::strcmp(name,"CreateFruityInstance"))return result;
    if(!imageAt((std::uintptr_t)result,s.app.enginePath,0xa410)
        && !imageAt((std::uintptr_t)result,s.app.enginePath,0x9ef0))return result;
    if(originalCreate&&originalCreate!=(Create)result)return result;originalCreate=(Create)result;return (void*)nativeCreate;
}
bool testAudioRequested()noexcept{return state().testModeRequested;}
bool permittedDevice(AudioDeviceID device,NativeOutputStartDiagnostic* diagnostic=nullptr)noexcept{auto& s=state();if(!testAudioRequested())return true;
    if(!s.enabled.load()||s.testDeviceUID!="Pro Tools Audio Bridge 2-A_UID"){s.refusedDeviceStarts.fetch_add(1);return false;}
    AudioObjectPropertyAddress uidAddress{kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};CFStringRef uid=nullptr;UInt32 size=sizeof(uid);
    AudioObjectPropertyAddress transportAddress{kAudioDevicePropertyTransportType,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 transport=0,transportSize=sizeof(transport);
    const auto uidStatus=AudioObjectGetPropertyData(device,&uidAddress,0,nullptr,&size,&uid);
    const bool validUID=uidStatus==noErr&&uid&&size==sizeof(uid);
    bool uidMatched=false;if(validUID){char value[2048];uidMatched=CFStringGetCString(uid,value,sizeof(value),kCFStringEncodingUTF8)&&s.testDeviceUID==value;}
    OSStatus transportStatus=0;const bool transportAttempted=validUID;
    if(transportAttempted)transportStatus=AudioObjectGetPropertyData(device,&transportAddress,0,nullptr,&transportSize,&transport);
    const bool virtualMatched=transportAttempted&&transportStatus==noErr&&transportSize==sizeof(transport)&&transport==kAudioDeviceTransportTypeVirtual;
    if(diagnostic){diagnostic->deviceID=device;diagnostic->uidReadAttempted=true;diagnostic->uidStatus=uidStatus;diagnostic->uidBytes=size;diagnostic->uidMatched=uidMatched;
        diagnostic->transportReadAttempted=transportAttempted;diagnostic->transportStatus=transportStatus;diagnostic->transportBytes=transportSize;diagnostic->virtualTransportMatched=virtualMatched;}
    const bool okay=validUID&&uidMatched&&virtualMatched;if(uid)CFRelease(uid);
    if(okay)s.permittedDeviceStarts.fetch_add(1);else s.refusedDeviceStarts.fetch_add(1);return okay;
}
// Restriction-only TLS: unrelated/rejected lifecycle calls cannot inherit an
// outer start's capture/permission authority. This NEVER grants permission.
thread_local std::uint32_t driverAuthoritySuppressed=0;
struct SuppressDriverAuthority {
    bool overflow=false;
    SuppressDriverAuthority()noexcept{if(driverAuthoritySuppressed==UINT32_MAX)overflow=true;else ++driverAuthoritySuppressed;}
    ~SuppressDriverAuthority(){if(!overflow)--driverAuthoritySuppressed;}
};
bool isActualAppleHAL(AudioUnit unit)noexcept{
    if(!unit)return false;AudioComponentDescription description{};
    const auto component=AudioComponentInstanceGetComponent(unit);
    return component&&AudioComponentGetDescription(component,&description)==noErr&&
        description.componentType==kAudioUnitType_Output&&description.componentSubType==kAudioUnitSubType_HALOutput&&
        description.componentManufacturer==kAudioUnitManufacturer_Apple;
}
bool actualCaptureDevice(AudioUnit unit,AudioDeviceID& device)noexcept{
    UInt32 bytes=sizeof(device);return isActualAppleHAL(unit)&&
        AudioUnitGetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,&bytes)==noErr&&
        bytes==sizeof(device)&&permittedDevice(device);
}
void deviceIOCodeIdentity(std::uintptr_t code,std::uint32_t& source,std::uint64_t& offset)noexcept{
    code=pointer_provenance::codeAddress(code);
    Dl_info image{};if(!code||!dladdr(reinterpret_cast<void*>(code),&image)||!image.dli_fname){source=0;return;}
    const auto base=reinterpret_cast<std::uintptr_t>(image.dli_fbase);if(code>=base)offset=code-base;
    source=state().appleProfileAdmitted.load()&&canonical(image.dli_fname)==state().appleComponentPath?1:2;
}
void recordDeviceIO(NativeAudioTestStats::DeviceIODiagnostic diagnostic)noexcept{
    try{auto& s=state();std::lock_guard<std::mutex> guard(s.deviceIODiagnosticMutex);
        diagnostic.sequence=s.deviceIODiagnostic.sequence==UINT64_MAX?UINT64_MAX:s.deviceIODiagnostic.sequence+1;s.deviceIODiagnostic=diagnostic;}catch(...){}
}
UnitGate::Pair actualCreatePair(std::uintptr_t caller,AudioDeviceIOProc callback)noexcept{
    const auto& s=state();if(!s.appleProfileAdmitted)return UnitGate::Pair::None;
    caller=pointer_provenance::codeAddress(caller);
    const auto code=pointer_provenance::codeAddress(reinterpret_cast<std::uintptr_t>(callback));
    if(imageAt(caller,s.appleComponentPath,0x38bcc)&&imageAt(code,s.appleComponentPath,0xe5cb0))return UnitGate::Pair::AUHAL;
    if(imageAt(caller,s.appleComponentPath,0x81b8)&&imageAt(code,s.appleComponentPath,0x9fd8))return UnitGate::Pair::Generic;
    return UnitGate::Pair::None;
}
UnitGate::Pair actualStartPair(std::uintptr_t caller,bool atTime)noexcept{
    const auto& s=state();if(!s.appleProfileAdmitted)return UnitGate::Pair::None;
    caller=pointer_provenance::codeAddress(caller);
    if(!atTime&&imageAt(caller,s.appleComponentPath,0x391c4))return UnitGate::Pair::AUHAL;
    if(imageAt(caller,s.appleComponentPath,atTime?0x9bdc:0x9bec))return UnitGate::Pair::Generic;
    return UnitGate::Pair::None;
}
OSStatus observedCreateIOProc(AudioDeviceID device,AudioDeviceIOProc callback,void* client,AudioDeviceIOProcID* out){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if(!testAudioRequested())return AudioDeviceCreateIOProcID(device,callback,client,out);
    auto& s=state();s.unitDeviceCounts[4].fetch_add(1,std::memory_order_relaxed);
    NativeAudioTestStats::DeviceIODiagnostic diagnostic;diagnostic.api=3;diagnostic.stage=5;diagnostic.deviceID=device;
    diagnostic.originalInvoked=true;diagnostic.suppressed=driverAuthoritySuppressed!=0;
    deviceIOCodeIdentity(caller,diagnostic.callerSource,diagnostic.callerOffset);deviceIOCodeIdentity(reinterpret_cast<std::uintptr_t>(callback),diagnostic.callbackSource,diagnostic.callbackOffset);
    const auto result=AudioDeviceCreateIOProcID(device,callback,client,out); // unchanged, ONCE
    if(result)s.unitDeviceCounts[5].fetch_add(1,std::memory_order_relaxed);
    if(nativeApplicationAdmitted()&&!driverAuthoritySuppressed&&s.appleProfileAdmitted){
        const auto pair=actualCreatePair(caller,callback);
        diagnostic.pair=static_cast<std::uint32_t>(pair);
        diagnostic.captured=s.unitDeviceGate.recordCreate(device,reinterpret_cast<std::uintptr_t>(callback),
            result==noErr&&out?reinterpret_cast<std::uintptr_t>(*out):0,result,pair);
    }else s.unitDeviceCounts[9].fetch_add(1,std::memory_order_relaxed);
    diagnostic.originalStatus=result;recordDeviceIO(diagnostic);return result;
}
OSStatus observedDestroyIOProc(AudioDeviceID device,AudioDeviceIOProcID callback){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if(!testAudioRequested())return AudioDeviceDestroyIOProcID(device,callback);
    auto& s=state();s.unitDeviceCounts[6].fetch_add(1,std::memory_order_relaxed);
    NativeAudioTestStats::DeviceIODiagnostic diagnostic;diagnostic.api=4;diagnostic.deviceID=device;
    deviceIOCodeIdentity(caller,diagnostic.callerSource,diagnostic.callerOffset);
    auto destruction=s.unitDeviceGate.beginDestroy(device,reinterpret_cast<std::uintptr_t>(callback));
    if(!destruction){diagnostic.stage=6;recordDeviceIO(diagnostic);s.unitDeviceCounts[7].fetch_add(1);return kAudioHardwareIllegalOperationError;}
    auto* sink=s.testSink.load(std::memory_order_acquire);
    test_output::FLTestOutputSink::LifecycleLease mutation;
    if(destruction.known()){
        const auto unit=reinterpret_cast<AudioUnit>(destruction.unit());
        if(!sink){diagnostic.stage=7;recordDeviceIO(diagnostic);s.unitDeviceCounts[7].fetch_add(1);return kAudioHardwareIllegalOperationError;}
        // Nested same-unit teardown already owns the exact per-unit mutation
        // reservation. This is cleanup-only and never permits any device start.
        if(!sink->unitMutationHeldByCurrentThread(unit)){
            mutation=sink->tryAcquireLifecycleMutation(unit);
            if(!mutation){diagnostic.stage=7;recordDeviceIO(diagnostic);s.unitDeviceCounts[7].fetch_add(1);return kAudioHardwareIllegalOperationError;}
        }
    }
    if(!destruction.commitRevoke()){diagnostic.stage=8;recordDeviceIO(diagnostic);s.unitDeviceCounts[7].fetch_add(1);return kAudioHardwareIllegalOperationError;}
    SuppressDriverAuthority suppressed;
    const auto result=AudioDeviceDestroyIOProcID(device,callback); // ONCE, under exact ID reservation and unit mutation lease
    if(result)s.unitDeviceCounts[10].fetch_add(1,std::memory_order_relaxed);
    diagnostic.stage=5;diagnostic.originalInvoked=true;diagnostic.originalStatus=result;recordDeviceIO(diagnostic);
    destruction.close();return result; // failed destroys remain revoked
}
bool approveNestedDeviceStart(AudioDeviceID device,AudioDeviceIOProcID callback,UnitGate::Pair pair,bool atTime,
    NativeAudioTestStats::DeviceIODiagnostic& diagnostic)noexcept{
    auto& s=state();diagnostic.stage=1;
    if(!nativeApplicationAdmitted()||!s.appleProfileAdmitted.load())return false;
    diagnostic.stage=2;if(driverAuthoritySuppressed)return false;
    diagnostic.stage=3;if(!permittedDevice(device))return false;
    diagnostic.stage=4;
    return s.unitDeviceGate.approveDeviceStart(device,reinterpret_cast<std::uintptr_t>(callback),pair,atTime);
}
OSStatus observedDeviceStart(AudioDeviceID device,AudioDeviceIOProcID callback){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if(!testAudioRequested())return AudioDeviceStart(device,callback);
    auto& s=state();s.unitDeviceCounts[0].fetch_add(1,std::memory_order_relaxed);
    NativeAudioTestStats::DeviceIODiagnostic diagnostic;diagnostic.api=1;diagnostic.deviceID=device;diagnostic.suppressed=driverAuthoritySuppressed!=0;
    const auto pair=actualStartPair(caller,false);diagnostic.pair=static_cast<std::uint32_t>(pair);deviceIOCodeIdentity(caller,diagnostic.callerSource,diagnostic.callerOffset);
    if(!approveNestedDeviceStart(device,callback,pair,false,diagnostic)){
        s.refusedDeviceStarts.fetch_add(1);s.unitDeviceCounts[8].fetch_add(1);recordDeviceIO(diagnostic);return kAudioHardwareIllegalOperationError;
    }
    s.unitDeviceCounts[2].fetch_add(1,std::memory_order_relaxed);
    const auto result=AudioDeviceStart(device,callback); // unchanged, ONCE, only proven unit IOProc
    if(result)s.unitDeviceCounts[3].fetch_add(1,std::memory_order_relaxed);
    s.unitDeviceGate.deviceStartReturned(device,reinterpret_cast<std::uintptr_t>(callback),result);
    diagnostic.stage=5;diagnostic.originalInvoked=true;diagnostic.originalStatus=result;recordDeviceIO(diagnostic);return result;
}
OSStatus observedDeviceStartAtTime(AudioDeviceID device,AudioDeviceIOProcID callback,AudioTimeStamp* when,UInt32 flags){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if(!testAudioRequested())return AudioDeviceStartAtTime(device,callback,when,flags);
    auto& s=state();s.unitDeviceCounts[1].fetch_add(1,std::memory_order_relaxed);
    NativeAudioTestStats::DeviceIODiagnostic diagnostic;diagnostic.api=2;diagnostic.deviceID=device;diagnostic.suppressed=driverAuthoritySuppressed!=0;
    const auto pair=actualStartPair(caller,true);diagnostic.pair=static_cast<std::uint32_t>(pair);deviceIOCodeIdentity(caller,diagnostic.callerSource,diagnostic.callerOffset);
    if(!approveNestedDeviceStart(device,callback,pair,true,diagnostic)){
        s.refusedDeviceStarts.fetch_add(1);s.unitDeviceCounts[8].fetch_add(1);recordDeviceIO(diagnostic);return kAudioHardwareIllegalOperationError;
    }
    s.unitDeviceCounts[2].fetch_add(1,std::memory_order_relaxed);
    const auto result=AudioDeviceStartAtTime(device,callback,when,flags); // unchanged, ONCE
    if(result)s.unitDeviceCounts[3].fetch_add(1,std::memory_order_relaxed);
    s.unitDeviceGate.deviceStartReturned(device,reinterpret_cast<std::uintptr_t>(callback),result);
    diagnostic.stage=5;diagnostic.originalInvoked=true;diagnostic.originalStatus=result;recordDeviceIO(diagnostic);return result;
}
// Sink calls this only AFTER acquiring its nonblocking mutation lease. No table
// claim is held across any native API. CurrentDevice capture precedes callback
// registration and is association-only, never permission to start IO.
OSStatus forwardUnitSetProperty(AudioUnit unit,AudioUnitPropertyID property,AudioUnitScope scope,
    AudioUnitElement element,const void* data,UInt32 bytes){
    auto& s=state();
    if(property==kAudioOutputUnitProperty_CurrentDevice){
        s.unitDeviceGate.revokeUnit(reinterpret_cast<std::uintptr_t>(unit));
        if(scope==kAudioUnitScope_Global&&element==0&&data&&bytes==sizeof(AudioDeviceID)&&
           nativeApplicationAdmitted()&&s.appleProfileAdmitted&&isActualAppleHAL(unit)){
            AudioDeviceID device=0;std::memcpy(&device,data,sizeof(device));
            if(permittedDevice(device)){
                auto capture=s.unitDeviceGate.beginCapture(UnitGate::Kind::CurrentDevice,reinterpret_cast<std::uintptr_t>(unit),device);
                if(!capture){s.unitDeviceCounts[7].fetch_add(1);return kAudioUnitErr_CannotDoInCurrentContext;}
                const auto result=AudioUnitSetProperty(unit,property,scope,element,data,bytes);
                if(result)s.unitDeviceGate.revokeUnit(reinterpret_cast<std::uintptr_t>(unit));return result;
            }
        }
    }
    SuppressDriverAuthority suppressed;return AudioUnitSetProperty(unit,property,scope,element,data,bytes);
}
OSStatus observedUnitSetProperty(AudioUnit unit,AudioUnitPropertyID property,AudioUnitScope scope,
    AudioUnitElement element,const void* data,UInt32 bytes){
    auto& s=state();auto* sink=s.testSink.load(std::memory_order_acquire);
    if(testAudioRequested()&&s.enabled.load()&&sink)return sink->observedSetProperty(unit,property,scope,element,data,bytes);
    return AudioUnitSetProperty(unit,property,scope,element,data,bytes);
}
OSStatus observedUnitInitialize(AudioUnit unit){
    if(!testAudioRequested())return AudioUnitInitialize(unit);
    auto& s=state();auto* sink=s.testSink.load(std::memory_order_acquire);
    if(!sink){SuppressDriverAuthority suppressed;return AudioUnitInitialize(unit);}
    auto mutation=sink->tryAcquireLifecycleMutation(unit);
    if(!mutation){s.unitDeviceCounts[7].fetch_add(1);return kAudioUnitErr_CannotDoInCurrentContext;}
    AudioDeviceID device=0;
    if(nativeApplicationAdmitted()&&s.appleProfileAdmitted&&actualCaptureDevice(unit,device)){
        auto capture=s.unitDeviceGate.beginCapture(UnitGate::Kind::Initialize,reinterpret_cast<std::uintptr_t>(unit),device);
        if(!capture){s.unitDeviceCounts[7].fetch_add(1);return kAudioUnitErr_CannotDoInCurrentContext;}
        const auto result=AudioUnitInitialize(unit);
        if(result)s.unitDeviceGate.revokeUnit(reinterpret_cast<std::uintptr_t>(unit));return result;
    }
    SuppressDriverAuthority suppressed;return AudioUnitInitialize(unit);
}
OSStatus observedUnitUninitialize(AudioUnit unit){
    if(!testAudioRequested())return AudioUnitUninitialize(unit);
    auto& s=state();auto* sink=s.testSink.load(std::memory_order_acquire);
    auto mutation=sink?sink->tryAcquireLifecycleMutation(unit):test_output::FLTestOutputSink::LifecycleLease{};
    if(sink&&!mutation){s.unitDeviceCounts[7].fetch_add(1);return kAudioUnitErr_CannotDoInCurrentContext;}
    s.unitDeviceGate.revokeUnit(reinterpret_cast<std::uintptr_t>(unit));
    SuppressDriverAuthority suppressed;return AudioUnitUninitialize(unit);
}
OSStatus observedComponentNew(AudioComponent component,AudioComponentInstance* instance){
    if(!testAudioRequested())return AudioComponentInstanceNew(component,instance);
    SuppressDriverAuthority suppressed;const auto result=AudioComponentInstanceNew(component,instance);
    if(!result&&instance&&*instance){
        auto& s=state();s.unitDeviceGate.revokeUnit(reinterpret_cast<std::uintptr_t>(*instance));
        if(auto* sink=s.testSink.load(std::memory_order_acquire)){
            auto retired=sink->tryAcquireLifecycleMutation(*instance,true,true);
            if(!retired)s.unitDeviceCounts[7].fetch_add(1);
        }
    }return result;
}
OSStatus observedComponentDispose(AudioComponentInstance instance){
    if(!testAudioRequested())return AudioComponentInstanceDispose(instance);
    auto& s=state();auto* sink=s.testSink.load(std::memory_order_acquire);
    auto mutation=sink?sink->tryAcquireLifecycleMutation(instance,true,true):test_output::FLTestOutputSink::LifecycleLease{};
    if(sink&&!mutation){s.unitDeviceCounts[7].fetch_add(1);return kAudioUnitErr_CannotDoInCurrentContext;}
    s.unitDeviceGate.revokeUnit(reinterpret_cast<std::uintptr_t>(instance));
    SuppressDriverAuthority suppressed;return AudioComponentInstanceDispose(instance);
}
void recordOutputStart(NativeOutputStartDiagnostic diagnostic)noexcept{
    try{auto& s=state();std::lock_guard<std::mutex> guard(s.outputStartDiagnosticMutex);
        diagnostic.sequence=s.outputStartDiagnostic.sequence==UINT64_MAX?UINT64_MAX:s.outputStartDiagnostic.sequence+1;s.outputStartDiagnostic=diagnostic;}catch(...){}
}
bool leasedSinkStillPrepared(void* context)noexcept{
    return context&&static_cast<test_output::FLTestOutputSink::OutputStartLease*>(context)->stillPrepared();
}
OSStatus observedOutputStart(AudioUnit unit){auto& s=state();if(testAudioRequested()){
    NativeOutputStartDiagnostic diagnostic;
    const auto refuse=[&](std::uint32_t stage){diagnostic.stage=stage;recordOutputStart(diagnostic);return kAudioHardwareIllegalOperationError;};
    if(!s.enabled.load()||s.testDeviceUID!="Pro Tools Audio Bridge 2-A_UID"){s.refusedDeviceStarts.fetch_add(1);return refuse(1);}
    AudioDeviceID device=0;UInt32 size=sizeof(device);
    diagnostic.currentDeviceStatus=AudioUnitGetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,&size);
    diagnostic.deviceID=device;diagnostic.currentDeviceBytes=size;
    if(diagnostic.currentDeviceStatus||size!=sizeof(device))return refuse(2);
    if(!permittedDevice(device,&diagnostic))return refuse(3);
    auto* sink=s.testSink.load(std::memory_order_acquire);if(!sink)return refuse(4);
    if(!sink->tryPrepareMutedOutputStart(unit))return refuse(5);
    test_output::FLTestOutputSink::PreparedOutputTicket ticket;
    if(!sink->capturePreparedTicket(unit,ticket))return refuse(7);
    auto lease=sink->tryAcquirePreparedStart(ticket);
    if(!lease||ticket.device()!=device)return refuse(7);
    if(!s.appleProfileAdmitted)return refuse(9);
    AudioDeviceID current=0;size=sizeof(current);
    diagnostic.currentDeviceStatus=AudioUnitGetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&current,&size);diagnostic.currentDeviceBytes=size;
    if(diagnostic.currentDeviceStatus||size!=sizeof(current))return refuse(2);
    if(current!=device)return refuse(10);
    if(!permittedDevice(current,&diagnostic))return refuse(3);
    auto capture=s.unitDeviceGate.beginOutputStart(reinterpret_cast<std::uintptr_t>(unit),device,ticket.generation(),leasedSinkStillPrepared,&lease);
    if(!capture)return refuse(8);
    diagnostic.originalInvoked=true;diagnostic.originalStatus=AudioOutputUnitStart(unit);
    s.unitDeviceGate.originalStartReturned(capture,diagnostic.originalStatus);
    if(diagnostic.originalStatus)lease.invalidateAfterFailedStart();
    capture.close(); // closes TLS authority BEFORE releasing the Sink lease
    diagnostic.stage=6;recordOutputStart(diagnostic);return diagnostic.originalStatus;
    }return AudioOutputUnitStart(unit);
}
bool testSinkAdmitted(void*)noexcept{return nativeApplicationAdmitted();}
bool testSinkDevice(AudioDeviceID device,void*)noexcept{return permittedDevice(device);}
} // namespace
bool initializeNativeApplication(const NativeApplicationCallbacks& callbacks)noexcept{try{auto& s=state();if(s.enabled.load())return false;
    const char* run=getenv("RYSUP_FLARA_RUN_ID"),*directory=getenv("RYSUP_FLARA_RUN_DIR"),*expected=getenv("RYSUP_FLARA_EXPECTED_EXE"),*parent=getenv("RYSUP_FLARA_EXPECTED_PARENT_PID");
    const char* uid=getenv("RYSUP_FLARA_TEST_AUDIO_DEVICE_UID");if(uid&&std::strcmp(uid,"Pro Tools Audio Bridge 2-A_UID"))return false;
    if(!run||!uuid(run)||!directory||!expected||!parent)return false;char* end=nullptr;const auto expectedParent=strtol(parent,&end,10);if(!end||*end||expectedParent<2||expectedParent!=getppid())return false;
    char executable[PATH_MAX];std::uint32_t size=sizeof(executable);if(_NSGetExecutablePath(executable,&size))return false;
    const auto exe=canonical(executable),folder=canonical(directory);if(exe.empty()||exe!=expected||folder.empty()||folder!=directory||exe.find(folder+"/")!=0||folder.substr(folder.rfind('/')+1)!=run)return false;
    struct stat st{};if(lstat(folder.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0022))return false;
    const std::string suffix="/Contents/MacOS/OsxFL";if(exe.size()<suffix.size()||exe.substr(exe.size()-suffix.size())!=suffix)return false;
    s.app.startupOwnershipVerified=true;s.app.processID=getpid();
    if(shaFile(exe)!=exeSHA)return false;
    const auto app=exe.substr(0,exe.size()-suffix.size());const auto engine=app+"/Contents/Libs/FLEngine_x64.dylib";const auto wrapper=app+"/Contents/Resources/FL/Plugins/Fruity/Effects/Fruity Wrapper/Fruity Wrapper_x64.dylib";
    if(shaFile(engine)!=engineSHA||shaFile(wrapper)!=wrapperSHA)return false;
    s.log=open((folder+"/native-app.jsonl").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(s.log<0)return false;
    s.app.runID=run;s.app.runDirectory=folder;s.app.executablePath=exe;s.app.executableSHA256=exeSHA;s.app.enginePath=engine;s.app.wrapperPath=wrapper;s.app.processID=getpid();s.app.parentProcessID=getppid();s.app.admitted=true;s.callbacks=callbacks;
    if(uid)s.testDeviceUID=uid;
    // Preserve native file-panel behavior. The OS panel service can share its
    // navigation history with the source application's domain. Do not force
    // private local panels: the native control failed normal saving on the
    // tested OS. Navigation history is distinct from DAW/audio configuration.
    if(const char* registry=getenv("RYSUP_FLARA_ORIGINAL_REGISTRY_PATH");registry&&*registry){
        RegistrationReadOptions options;options.originalRegistryPath=registry;
        options.exactWinBridgePath=app+"/Contents/Resources/FL/Shared/libwinbridge_x64.dylib";
        if(!initializeRegistrationReadThrough(options)){
            const auto diagnostic=registrationReadStats();
            writeNativeApplicationEvent("registration_read_through_unavailable",
                "stage="+std::to_string(static_cast<std::uint32_t>(diagnostic.stage))+
                " errno="+std::to_string(diagnostic.immediateErrno));
        }
    }
    s.enabled.store(true);
    if(uid){
        s.appleComponentPath=canonical(appleAudioComponent);
        s.appleProfileAdmitted=s.appleComponentPath==appleAudioComponent&&shaFile(s.appleComponentPath)==appleAudioComponentSHA;
        auto* sink=new(std::nothrow)test_output::FLTestOutputSink;
        test_output::Config config;config.exactAdmittedEnginePath=s.app.enginePath.c_str();config.exactTestDeviceUID=s.testDeviceUID.c_str();
        config.functions.setProperty=forwardUnitSetProperty;
        config.functions.currentlyAdmitted=testSinkAdmitted;config.functions.verifyExactTestDevice=testSinkDevice;
        config.expectedOutputCallbackOffset=0xcd7bc0;
        if(!sink||!sink->initialize(config)){delete sink;s.enabled=false;return false;}
        // The output unit and every callback record are process-lifetime. Test
        // audio reaches genuine native rendering, then only the driver-facing
        // buffers are silenced; offline file output remains upstream.
        s.testSink.store(sink,std::memory_order_release);
    }
    writeNativeApplicationEvent("owned_normal_fl_admitted");if(s.callbacks.admitted)s.callbacks.admitted(s.app,s.callbacks.context);return true;
}catch(...){return false;}}
const NativeApplication& nativeApplication()noexcept{return state().app;}
NativeAudioTestStats nativeAudioTestStats()noexcept{
    NativeAudioTestStats out;auto& s=state();try{std::lock_guard<std::mutex> guard(s.outputStartDiagnosticMutex);out.lastOutputStart=s.outputStartDiagnostic;}catch(...){}
    auto* sink=s.testSink.load(std::memory_order_acquire);if(!sink)return out;
    const auto value=sink->snapshot();out.presentationMuteConfigured=true;
    out.callbackCalls=value.callbackCalls;out.nonfiniteScratchSamples=value.nonfiniteScratchSamples;out.preparedUnits=value.preparedUnits;
    out.nativeRenderCalls=value.nativeRenderCalls;out.nativeRenderedFrames=value.nativeRenderedFrames;
    out.mutedFrames=value.mutedFrames;out.nonzeroScratchSamples=value.nonzeroScratchSamples;
    out.nativeErrors=value.nativeErrors;out.unsupportedCallbacks=value.unsupportedCallbacks;
    out.concurrentCallbacks=value.concurrentCallbacks;out.unsafeHardwareBuffers=value.unsafeHardwareBuffers;
    out.returnedShapeFailures=value.returnedShapeFailures;out.counterOverflows=value.counterOverflows;
    out.startRefusals=value.startRefusals;out.registrationRefusals=value.registrationRefusals;
    out.writeOnlyCallbackPreparations=value.writeOnlyCallbackPreparations;
    out.sinkLifecycleRefusals=value.lifecycleRefusals;
    out.appleDeviceProfileAdmitted=state().appleProfileAdmitted;
    out.deviceStartEntries=state().unitDeviceCounts[0].load();out.deviceStartAtTimeEntries=state().unitDeviceCounts[1].load();
    out.deviceStartOriginalCalls=state().unitDeviceCounts[2].load();out.deviceStartOriginalFailures=state().unitDeviceCounts[3].load();
    out.ioProcCreateEntries=state().unitDeviceCounts[4].load();out.ioProcCreateOriginalFailures=state().unitDeviceCounts[5].load();
    out.ioProcDestroyEntries=state().unitDeviceCounts[6].load();out.lifecycleRefusals=state().unitDeviceCounts[7].load();
    out.unitDeviceGate=state().unitDeviceGate.snapshot();
    out.deviceStartRefusals=state().unitDeviceCounts[8].load();out.ioProcCaptureSuppressed=state().unitDeviceCounts[9].load();out.ioProcDestroyOriginalFailures=state().unitDeviceCounts[10].load();
    {std::lock_guard<std::mutex> guard(state().deviceIODiagnosticMutex);out.lastDeviceIO=state().deviceIODiagnostic;}
    out.criticalPropertyDisarms=value.criticalPropertyDisarms;
    const auto& registration=value.lastRegistration;auto& destination=out.lastRegistration;
    destination.stage=static_cast<std::uint32_t>(registration.stage);destination.reason=static_cast<std::uint32_t>(registration.reason);
    destination.property=registration.property;destination.scope=registration.scope;destination.element=registration.element;destination.bytes=registration.bytes;
    destination.callbackSource=static_cast<std::uint32_t>(registration.callbackSource);destination.callbackImageOffset=registration.callbackImageOffset;
    destination.sequence=registration.sequence;destination.dropped=registration.dropped;destination.originalStatus=registration.originalStatus;
    destination.recordPresent=registration.recordPresent;destination.claimHeld=registration.claimHeld;destination.originalInvoked=registration.originalInvoked;destination.coherent=registration.coherent;
    const auto& metadata=value.callbackMetadata;auto& output=out.callbackMetadata;
    output.registeredScope=metadata.registeredScope;output.registeredElement=metadata.registeredElement;
    output.routeGetterStatus=metadata.routeGetterStatus;output.routeGetterBytes=metadata.routeGetterBytes;
    output.routeInfoStatus=metadata.routeInfoStatus;output.routeInfoBytes=metadata.routeInfoBytes;output.routeWritable=metadata.routeWritable;
    output.inputGetterStatus=metadata.inputGetterStatus;output.inputGetterBytes=metadata.inputGetterBytes;
    output.inputInfoStatus=metadata.inputInfoStatus;output.inputInfoBytes=metadata.inputInfoBytes;output.inputWritable=metadata.inputWritable;
    output.outputAlias=metadata.outputAlias;output.accepted=metadata.accepted;output.coherent=metadata.coherent;
    output.routeQueried=metadata.routeQueried;output.inputQueried=metadata.inputQueried;output.sequence=metadata.sequence;output.dropped=metadata.dropped;
    return out;
}
bool nativeApplicationAdmitted()noexcept{
    // Parent identity is verified before enabling the bridge. A launcher that
    // later exits leaves this SAME admitted native process and its sandbox
    // alive; orphaning must not silently turn off archive unwrap/save handling.
    // A fork does not inherit admission: its PID differs from the captured PID.
    return state().enabled.load()&&state().app.admitted&&state().app.processID==getpid();
}
bool refreshActualNativeApplication()noexcept{return refreshEngine();}
void writeNativeApplicationEvent(const char* event,const std::string& detail)noexcept{try{auto& s=state();if(s.log<0||!event||!pthread_main_np())return;const auto line="{\"event\":"+quoted(event)+",\"run_id\":"+quoted(s.app.runID)+",\"pid\":"+std::to_string(getpid())+",\"detail\":"+quoted(detail)+"}\n";const char* p=line.data();std::size_t n=line.size();while(n){const auto done=write(s.log,p,n);if(done<0&&errno==EINTR)continue;if(done<=0)break;p+=done;n-=std::size_t(done);}}catch(...){}}
bool resolveActualNativeInsert(std::uintptr_t component,std::uintptr_t context,NativeInsert& out)noexcept{auto& s=state();if(!nativeApplicationAdmitted()||!pthread_main_np()||!refreshEngine()||!component||context<0x18)return false;NativeInsert v;v.actualComponent=component;v.initializeContext=context;v.hostInstance=context-0x18;
    if(!objectAt(context,s.app.wrapperPath,0x2b3dc8)||!objectAt(v.hostInstance,s.app.wrapperPath,0x2b0010)||!readAt(v.hostInstance+8,v.plugin)||!objectAt(v.plugin,s.app.wrapperPath,0x2afb80)||!readAt(v.plugin+0x70,v.audioPluginHost)||!objectAt(v.audioPluginHost,s.app.wrapperPath,0x2a9968)||!readAt(v.audioPluginHost+8,v.effectWrapper)||!objectAt(v.effectWrapper,s.app.wrapperPath,0x2aaa70)||!readAt(v.effectWrapper+8,v.tag)||!objectAt(v.tag,s.app.enginePath,0x13e2c88)||!readAt(v.effectWrapper+0xc0,v.host)||!objectAt(v.host,s.app.enginePath,0x140a818))return false;
    std::uintptr_t reverseHost=0,reverseManager=0,currentComponent=0;
    if(!wrapperAssociationSealed(context)||!readAt(v.plugin+0x818,currentComponent)||currentComponent!=component
        ||!readAt(v.plugin+0x7a8,reverseHost)||reverseHost!=v.hostInstance||!readAt(v.effectWrapper+0x50b0,reverseManager)
        ||reverseManager!=v.audioPluginHost||!readAt(v.tag+0x120,v.mixerInsert)||v.mixerInsert<0||v.mixerInsert>500)return false;
    v.evidence="actual native initialize context; sealed reciprocal TVST3HostInstance/TVST3Plugin/TAudioPluginHost/TEffectWrapper/native Tag/mixer chain; current P+818 equals actual component";out=std::move(v);return true;
}
bool revalidateActualNativeInsert(const NativeInsert& expected)noexcept{NativeInsert current;return resolveActualNativeInsert(expected.actualComponent,expected.initializeContext,current)&&current.hostInstance==expected.hostInstance&&current.plugin==expected.plugin&&current.audioPluginHost==expected.audioPluginHost&&current.effectWrapper==expected.effectWrapper&&current.tag==expected.tag&&current.host==expected.host&&current.mixerInsert==expected.mixerInsert;}
bool resolveActualNativeController(std::uintptr_t controller,std::uintptr_t context,NativeInsert& out)noexcept{
    if(!pthread_main_np()||!nativeApplicationAdmitted()||!controller||context<0x18)return false;
    std::uintptr_t plugin=0,component=0,actualController=0;
    if(!readAt(context-0x18+8,plugin)||!plugin||!readAt(plugin+0x818,component)||!component
        ||!readAt(plugin+0x828,actualController)||actualController!=controller)return false;
    NativeInsert insert;if(!resolveActualNativeInsert(component,context,insert)||insert.plugin!=plugin)return false;
    std::uintptr_t repeatedComponent=0,repeatedController=0;
    if(!readAt(plugin+0x818,repeatedComponent)||repeatedComponent!=component
        ||!readAt(plugin+0x828,repeatedController)||repeatedController!=controller)return false;
    insert.evidence+="; current P+828 equals separately observed actual native controller";out=std::move(insert);return true;
}
bool revalidateActualNativeController(const NativeInsert& expected,std::uintptr_t controller)noexcept{
    NativeInsert current;return resolveActualNativeController(controller,expected.initializeContext,current)
        &&current.actualComponent==expected.actualComponent&&current.hostInstance==expected.hostInstance&&current.plugin==expected.plugin
        &&current.audioPluginHost==expected.audioPluginHost&&current.effectWrapper==expected.effectWrapper
        &&current.tag==expected.tag&&current.host==expected.host&&current.mixerInsert==expected.mixerInsert;
}
bool readActualNativeTransportState(std::int32_t& value)noexcept{const auto& app=state().app;if(!pthread_main_np()||!nativeApplicationAdmitted()||!refreshEngine())return false;std::int32_t second=0;return readAt(app.engine.imageBase+0x21a2354,value)&&readAt(app.engine.imageBase+0x21a2354,second)&&value==second&&value>=0&&value<=2;}
bool withActualNativeMixCaptureLock(const NativeInsert& insert,const std::function<void()>& copy){if(!pthread_main_np()||!copy||!revalidateActualNativeInsert(insert))return false;std::int32_t transport=-1;if(!readActualNativeTransportState(transport)||transport!=0)return false;std::uintptr_t table=0,lock=0,unlock=0;if(!readAt(insert.host,table)||!readAt(table+0x100,lock)||!readAt(table+0x108,unlock)||!imageAt(lock,state().app.enginePath,0x42cd90)||!imageAt(unlock,state().app.enginePath,0x42cdb0))return false;
    const auto criticalGlobal=state().app.engine.imageBase+0x1f20ab8;std::uintptr_t critical=0,criticalTable=0,criticalLock=0,criticalUnlock=0;
    if(!readAt(criticalGlobal,critical)||!objectAt(critical,state().app.enginePath,0x17187e0)||!readAt(critical,criticalTable)||!readAt(criticalTable+0xc8,criticalLock)||!readAt(criticalTable+0xd0,criticalUnlock)||!imageAt(criticalLock,state().app.enginePath,0xa7d380)||!imageAt(criticalUnlock,state().app.enginePath,0xa7d3a0))return false;
    const auto sameCritical=[&]{std::uintptr_t current=0;return readAt(criticalGlobal,current)&&current==critical&&objectAt(critical,state().app.enginePath,0x17187e0);};
    using Function=void(*)(void*);((Function)lock)((void*)insert.host);struct Unlock{Function fn;void* host;~Unlock(){fn(host);}}done{(Function)unlock,(void*)insert.host};if(!sameCritical()||!readActualNativeTransportState(transport)||transport!=0||!revalidateActualNativeInsert(insert))return false;copy();return sameCritical()&&readActualNativeTransportState(transport)&&transport==0&&revalidateActualNativeInsert(insert);
}
bool requestActualNativePlaying(const NativeInsert& insert,bool playing)noexcept{if(!pthread_main_np()||!revalidateActualNativeInsert(insert))return false;std::int32_t current=-1;if(!readActualNativeTransportState(current)||(current!=0&&current!=1))return false;if((current==1)==playing)return true;std::uintptr_t table=0,dispatch=0;if(!readAt(insert.host,table)||!readAt(table+0xc8,dispatch)||!imageAt(dispatch,state().app.enginePath,0x425f60))return false;
    // Public FL SDK TFruityPlugHost::Dispatcher(host,Tag,ID,Index,Value).
    using Dispatcher=std::intptr_t(*)(void*,std::intptr_t,std::intptr_t,std::intptr_t,std::intptr_t);((Dispatcher)dispatch)((void*)insert.host,(std::intptr_t)insert.tag,79,playing?10:11,playing?2:1);
    return readActualNativeTransportState(current)&&current==(playing?1:0);
}
bool requestActualNativeProjectDirty(const NativeInsert& insert)noexcept{
    if(!pthread_main_np()||!revalidateActualNativeInsert(insert))return false;
    std::uintptr_t table=0,dispatch=0;
    if(!readAt(insert.host,table)||!readAt(table+0xc8,dispatch)
        ||!imageAt(dispatch,state().app.enginePath,0x425f60))return false;
    // fp_plugclass.h: FHD_SetDirty=48 marks non-automatable changes. No native
    // process method, fabricated parameter edit or vendor-specific call.
    using Dispatcher=std::intptr_t(*)(void*,std::intptr_t,std::intptr_t,std::intptr_t,std::intptr_t);
    ((Dispatcher)dispatch)((void*)insert.host,(std::intptr_t)insert.tag,48,0,0);
    return true;
}
} // namespace rysup::fl_ara
struct FLAppInterpose{const void* replacement;const void* original;};
__attribute__((used,section("__DATA,__interpose")))static const FLAppInterpose flAppInterposes[]={
 {(const void*)rysup::fl_ara::observedDlsym,(const void*)dlsym},
 {(const void*)rysup::fl_ara::observedDeviceStart,(const void*)AudioDeviceStart},
 {(const void*)rysup::fl_ara::observedDeviceStartAtTime,(const void*)AudioDeviceStartAtTime},
 {(const void*)rysup::fl_ara::observedOutputStart,(const void*)AudioOutputUnitStart},
 {(const void*)rysup::fl_ara::observedUnitSetProperty,(const void*)AudioUnitSetProperty},
 {(const void*)rysup::fl_ara::observedCreateIOProc,(const void*)AudioDeviceCreateIOProcID},
 {(const void*)rysup::fl_ara::observedDestroyIOProc,(const void*)AudioDeviceDestroyIOProcID},
 {(const void*)rysup::fl_ara::observedUnitInitialize,(const void*)AudioUnitInitialize},
 {(const void*)rysup::fl_ara::observedUnitUninitialize,(const void*)AudioUnitUninitialize},
 {(const void*)rysup::fl_ara::observedComponentNew,(const void*)AudioComponentInstanceNew},
 {(const void*)rysup::fl_ara::observedComponentDispose,(const void*)AudioComponentInstanceDispose}
};
