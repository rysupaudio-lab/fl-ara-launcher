// Process-local normal FL application admission. Original installed binaries,
// vendor signatures and native creation arguments are never changed.
#include "FLNativeApplication.h"
#include "RegistrationReadThrough.h"
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

namespace rysup::fl_ara { namespace {
constexpr const char* exeSHA="5be78aa9f9f066f3b28747b48357769b70979e859b42661f2af7163f08e57aab";
constexpr const char* engineSHA="8d23eebf0fc63fdd1957ba2c8d0f1f1d4e053a9f43173d3c22606c47ace7b76d";
constexpr const char* wrapperSHA="445dd1766e95013acc5046d7f08346fa04ef451a5d16e035a800518199c4d456";
struct State {
    NativeApplication app;NativeApplicationCallbacks callbacks;
    CFRunLoopTimerRef timer=nullptr;int log=-1;void** actualOutArgument=nullptr;
    std::string testDeviceUID;std::atomic<bool> enabled{false};
    std::atomic<std::uint64_t> permittedDeviceStarts{0},refusedDeviceStarts{0};
};
State& state(){static State value;return value;}
std::string canonical(const char* p){char value[PATH_MAX];return p&&realpath(p,value)?value:"";}
std::string hex(const void* bytes,std::size_t size){static const char* digits="0123456789abcdef";std::string s(size*2,'0');auto p=(const unsigned char*)bytes;for(std::size_t i=0;i<size;++i){s[i*2]=digits[p[i]>>4];s[i*2+1]=digits[p[i]&15];}return s;}
std::string shaFile(const std::string& path){int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return{};struct stat a{},b{};if(fstat(fd,&a)||!S_ISREG(a.st_mode)){close(fd);return{};}CC_SHA256_CTX hash;CC_SHA256_Init(&hash);unsigned char bytes[65536];ssize_t n=0;while((n=read(fd,bytes,sizeof(bytes)))>0)CC_SHA256_Update(&hash,bytes,(CC_LONG)n);const bool okay=n==0&&!fstat(fd,&b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size&&a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec&&a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec;close(fd);if(!okay)return{};unsigned char digest[32];CC_SHA256_Final(digest,&hash);return hex(digest,32);}
template<class T>bool readAt(std::uintptr_t p,T& out)noexcept{mach_vm_size_t count=0;return p&&mach_vm_read_overwrite(mach_task_self(),p,sizeof(T),(mach_vm_address_t)&out,&count)==KERN_SUCCESS&&count==sizeof(T);}
bool imageAt(std::uintptr_t p,const std::string& path,std::uintptr_t offset)noexcept{Dl_info image{};return p&&dladdr((void*)p,&image)&&canonical(image.dli_fname)==path&&p-(std::uintptr_t)image.dli_fbase==offset;}
bool objectAt(std::uintptr_t p,const std::string& path,std::uintptr_t vmt)noexcept{std::uintptr_t table=0;return readAt(p,table)&&imageAt(table,path,vmt);}
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
    if(!s.enabled.load()||!pthread_main_np()||s.app.creationEntered||!host||!out)return fn(host,out);
    s.app.creationEntered=true;s.app.startupHost=(std::uintptr_t)host;s.actualOutArgument=out;
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
bool testAudioRequested()noexcept{return getenv("RYSUP_FLARA_TEST_AUDIO_DEVICE_UID")!=nullptr;}
bool permittedDevice(AudioDeviceID device)noexcept{auto& s=state();if(!testAudioRequested())return true;
    if(!s.enabled.load()||s.testDeviceUID!="Pro Tools Audio Bridge 2-A_UID"){s.refusedDeviceStarts.fetch_add(1);return false;}
    AudioObjectPropertyAddress uidAddress{kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};CFStringRef uid=nullptr;UInt32 size=sizeof(uid);
    AudioObjectPropertyAddress transportAddress{kAudioDevicePropertyTransportType,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 transport=0,transportSize=sizeof(transport);
    bool okay=AudioObjectGetPropertyData(device,&uidAddress,0,nullptr,&size,&uid)==noErr&&uid&&size==sizeof(uid)&&AudioObjectGetPropertyData(device,&transportAddress,0,nullptr,&transportSize,&transport)==noErr&&transport==kAudioDeviceTransportTypeVirtual;
    if(okay){char value[2048];okay=CFStringGetCString(uid,value,sizeof(value),kCFStringEncodingUTF8)&&s.testDeviceUID==value;}if(uid)CFRelease(uid);
    if(okay)s.permittedDeviceStarts.fetch_add(1);else s.refusedDeviceStarts.fetch_add(1);return okay;
}
OSStatus observedDeviceStart(AudioDeviceID device,AudioDeviceIOProcID callback){return permittedDevice(device)?AudioDeviceStart(device,callback):kAudioHardwareIllegalOperationError;}
OSStatus observedDeviceStartAtTime(AudioDeviceID device,AudioDeviceIOProcID callback,AudioTimeStamp* when,UInt32 flags){return permittedDevice(device)?AudioDeviceStartAtTime(device,callback,when,flags):kAudioHardwareIllegalOperationError;}
OSStatus observedOutputStart(AudioUnit unit){auto& s=state();if(testAudioRequested()){if(!s.enabled.load()||s.testDeviceUID!="Pro Tools Audio Bridge 2-A_UID"){s.refusedDeviceStarts.fetch_add(1);return kAudioHardwareIllegalOperationError;}AudioDeviceID device=0;UInt32 size=sizeof(device);if(AudioUnitGetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,&size)||size!=sizeof(device)||!permittedDevice(device))return kAudioHardwareIllegalOperationError;}return AudioOutputUnitStart(unit);}
} // namespace
bool initializeNativeApplication(const NativeApplicationCallbacks& callbacks)noexcept{try{auto& s=state();if(s.enabled.load())return false;
    const char* run=getenv("RYSUP_FLARA_RUN_ID"),*directory=getenv("RYSUP_FLARA_RUN_DIR"),*expected=getenv("RYSUP_FLARA_EXPECTED_EXE"),*parent=getenv("RYSUP_FLARA_EXPECTED_PARENT_PID");
    const char* uid=getenv("RYSUP_FLARA_TEST_AUDIO_DEVICE_UID");if(uid&&std::strcmp(uid,"Pro Tools Audio Bridge 2-A_UID"))return false;
    if(!run||!uuid(run)||!directory||!expected||!parent)return false;char* end=nullptr;const auto expectedParent=strtol(parent,&end,10);if(!end||*end||expectedParent<2||expectedParent!=getppid())return false;
    char executable[PATH_MAX];std::uint32_t size=sizeof(executable);if(_NSGetExecutablePath(executable,&size))return false;
    const auto exe=canonical(executable),folder=canonical(directory);if(exe.empty()||exe!=expected||folder.empty()||folder!=directory||exe.find(folder+"/")!=0||folder.substr(folder.rfind('/')+1)!=run)return false;
    struct stat st{};if(lstat(folder.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0022))return false;
    const std::string suffix="/Contents/MacOS/OsxFL";if(exe.size()<suffix.size()||exe.substr(exe.size()-suffix.size())!=suffix||shaFile(exe)!=exeSHA)return false;
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
        if(!initializeRegistrationReadThrough(options))
            writeNativeApplicationEvent("registration_read_through_unavailable");
    }
    s.enabled.store(true);writeNativeApplicationEvent("owned_normal_fl_admitted");if(s.callbacks.admitted)s.callbacks.admitted(s.app,s.callbacks.context);return true;
}catch(...){return false;}}
const NativeApplication& nativeApplication()noexcept{return state().app;}
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
    std::uintptr_t reverseHost=0,reverseManager=0;if(!readAt(v.plugin+0x7a8,reverseHost)||reverseHost!=v.hostInstance||!readAt(v.effectWrapper+0x50b0,reverseManager)||reverseManager!=v.audioPluginHost||!readAt(v.tag+0x120,v.mixerInsert)||v.mixerInsert<0||v.mixerInsert>500)return false;
    v.evidence="actual native initialize context; reciprocal TVST3HostInstance/TVST3Plugin/TAudioPluginHost/TEffectWrapper/native Tag/mixer chain";out=std::move(v);return true;
}
bool revalidateActualNativeInsert(const NativeInsert& expected)noexcept{NativeInsert current;return resolveActualNativeInsert(expected.actualComponent,expected.initializeContext,current)&&current.hostInstance==expected.hostInstance&&current.plugin==expected.plugin&&current.audioPluginHost==expected.audioPluginHost&&current.effectWrapper==expected.effectWrapper&&current.tag==expected.tag&&current.host==expected.host&&current.mixerInsert==expected.mixerInsert;}
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
} // namespace rysup::fl_ara
struct FLAppInterpose{const void* replacement;const void* original;};
__attribute__((used,section("__DATA,__interpose")))static const FLAppInterpose flAppInterposes[]={
 {(const void*)rysup::fl_ara::observedDlsym,(const void*)dlsym},
 {(const void*)rysup::fl_ara::observedDeviceStart,(const void*)AudioDeviceStart},
 {(const void*)rysup::fl_ara::observedDeviceStartAtTime,(const void*)AudioDeviceStartAtTime},
 {(const void*)rysup::fl_ara::observedOutputStart,(const void*)AudioOutputUnitStart}
};
