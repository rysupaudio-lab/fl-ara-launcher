#include "FLHeadlessNativeExport.h"
#include "FLNativeApplication.h"
#include "FLNativeSaveGuard.h"
#import <Foundation/Foundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <dlfcn.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <array>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <limits.h>
namespace rysup::fl_ara { namespace {
std::atomic<bool> inFlight{false};
struct State {bool inspected=false,requested=false,attempted=false;NSString* root=nil;NSDictionary* request=nil;};
State& state(){static State* s=new State;return *s;}
void require(bool okay,const char* error){if(!okay)throw std::runtime_error(error);}
std::string canonical(const char* p){char b[PATH_MAX];return p&&realpath(p,b)?b:"";}
NSString* text(const std::string& v){return [NSString stringWithUTF8String:v.c_str()];}
std::string hashFile(const char* path){
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);require(fd>=0,"Cannot read owned export input/output");
    struct Close{int fd;~Close(){close(fd);}} closeFile{fd};struct stat a{},b{};
    require(!fstat(fd,&a)&&S_ISREG(a.st_mode)&&a.st_uid==getuid(),"Export input/output is not an owned regular file");
    CC_SHA256_CTX h;CC_SHA256_Init(&h);std::array<unsigned char,65536> block{};ssize_t n;
    while((n=read(fd,block.data(),block.size()))>0)CC_SHA256_Update(&h,block.data(),static_cast<CC_LONG>(n));
    require(n==0&&!fstat(fd,&b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size&&a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec&&a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec,"Export file changed while hashing");
    unsigned char digest[32];CC_SHA256_Final(digest,&h);std::string out;constexpr char hex[]="0123456789abcdef";
    for(auto v:digest){out+=hex[v>>4];out+=hex[v&15];}return out;
}
NSData* readRequest(NSString* path){
    int fd=open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);require(fd>=0,"Owned export request unreadable");
    struct stat st{};if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)||st.st_size<2||st.st_size>16384){close(fd);throw std::runtime_error("Owned export request has invalid type, permissions or size");}
    NSMutableData* data=[NSMutableData dataWithLength:static_cast<NSUInteger>(st.st_size)];size_t at=0;
    while(at<data.length){ssize_t n=read(fd,static_cast<char*>(data.mutableBytes)+at,data.length-at);if(n<=0){close(fd);throw std::runtime_error("Owned export request short read");}at+=static_cast<size_t>(n);}close(fd);return data;
}
template<class T>bool readAt(std::uintptr_t p,T& v){mach_vm_size_t n=0;return p&&mach_vm_read_overwrite(mach_task_self(),p,sizeof(v),reinterpret_cast<mach_vm_address_t>(&v),&n)==KERN_SUCCESS&&n==sizeof(v);}
void verifyCode(const NativeApplication& app){
    require(app.engine.verifiedArm64SliceSHA256==rysup::ara_lab::kFLNativeModelArm64SHA256&&app.engine.exactLoadedImagePath==app.enginePath,"Native export engine binding differs");
    const auto base=app.engine.imageBase;require(base&&base<UINTPTR_MAX-0x36ff80,"Native export image base invalid");
    Dl_info image{};require(dladdr(reinterpret_cast<void*>(base+0x3211c0),&image)&&reinterpret_cast<std::uintptr_t>(image.dli_fbase)==base&&canonical(image.dli_fname)==app.enginePath,"Native export image identity differs");
    constexpr std::array<std::uint32_t,8> expected{0xa9bf7bfd,0x910003fd,0xa9bf4ff3,0xd10783ff,0xf90003e0,0xb9000be1,0xf9000be2,0xf9000fe3};
    for(size_t i=0;i<expected.size();++i){std::uint32_t value=0;require(readAt(base+0x3211c0+i*4,value)&&value==expected[i],"Native export entry instructions differ");}
}
// Exact inspected FPC 3.2 immutable UnicodeString layout. Native stores this
// value globally; deliberately retain this one allocation through process exit.
std::uintptr_t retainedLiteral(NSString* path){
    const NSUInteger n=path.length;require(n>0&&n<=4096,"Owned export UTF16 extent invalid");
    for(NSUInteger i=0;i<n;++i){unichar c=[path characterAtIndex:i];require(c!=0,"Owned export path contains NUL");
        if(c>=0xd800&&c<=0xdbff){require(++i<n,"Unpaired export surrogate");unichar low=[path characterAtIndex:i];require(low>=0xdc00&&low<=0xdfff,"Unpaired export surrogate");}
        else require(c<0xdc00||c>0xdfff,"Unpaired export surrogate");}
    auto* b=static_cast<unsigned char*>(calloc(1,24+(n+1)*2));require(b,"Owned export path allocation failed");
    const std::uint16_t page=1200,width=2;const std::int64_t refs=-1,length=static_cast<std::int64_t>(n);
    std::memcpy(b,&page,2);std::memcpy(b+2,&width,2);std::memcpy(b+8,&refs,8);std::memcpy(b+16,&length,8);
    [path getCharacters:reinterpret_cast<unichar*>(b+24) range:NSMakeRange(0,n)];return reinterpret_cast<std::uintptr_t>(b+24);
}
void receipt(NSDictionary* extra){
    auto& s=state();if(!s.root)return;const auto& app=nativeApplication();
    NSMutableDictionary* row=[@{@"schema":@"rysup.fl-ara-owned-export-result/1",@"run_id":text(app.runID),@"pid":@(getpid()),@"requested":@(s.requested),@"attempted":@(s.attempted),@"native_audio_qualified":@NO,@"automatic_save_or_quit":@NO} mutableCopy];
    [row addEntriesFromDictionary:extra];NSData* data=[NSJSONSerialization dataWithJSONObject:row options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:nil];
    NSString* path=[s.root stringByAppendingPathComponent:@"headless-export-result.json"];
    if(data){[data writeToFile:path options:NSDataWritingAtomic error:nil];chmod(path.fileSystemRepresentation,0600);}
}
NSDictionary* verifyOutput(NSString* output){
    struct stat st{};require(!lstat(output.fileSystemRepresentation,&st)&&S_ISREG(st.st_mode)&&st.st_uid==getuid()&&st.st_size>44,"Native export did not create an owned WAV");
    AudioFileID file=nullptr;require(AudioFileOpenURL((__bridge CFURLRef)[NSURL fileURLWithPath:output],kAudioFileReadPermission,kAudioFileWAVEType,&file)==noErr&&file,"Native output is not readable WAVE audio");
    struct Close{AudioFileID f;~Close(){AudioFileClose(f);}} closeFile{file};AudioFileTypeID fileType=0;UInt32 size=sizeof(fileType);
    require(AudioFileGetProperty(file,kAudioFilePropertyFileFormat,&size,&fileType)==noErr&&fileType==kAudioFileWAVEType,"Native output container is not WAVE");
    AudioStreamBasicDescription format{};size=sizeof(format);
    require(AudioFileGetProperty(file,kAudioFilePropertyDataFormat,&size,&format)==noErr&&size==sizeof(format)&&format.mFormatID==kAudioFormatLinearPCM&&format.mSampleRate==48000&&format.mChannelsPerFrame==2&&format.mFramesPerPacket==1&&format.mBytesPerFrame>0,"Native WAV format differs from stereo 48k PCM");
    UInt64 packets=0,bytes=0;size=sizeof(packets);require(AudioFileGetProperty(file,kAudioFilePropertyAudioDataPacketCount,&size,&packets)==noErr&&packets>0,"Native WAV has no frames");
    size=sizeof(bytes);require(AudioFileGetProperty(file,kAudioFilePropertyAudioDataByteCount,&size,&bytes)==noErr&&packets<=UINT64_MAX/format.mBytesPerFrame&&bytes==packets*format.mBytesPerFrame,"Native WAV data extent invalid");
    const auto hash=hashFile(output.fileSystemRepresentation);
    return @{@"output":output,@"output_sha256":text(hash),@"output_file_bytes":@(st.st_size),@"sample_rate":@(format.mSampleRate),@"channels":@(format.mChannelsPerFrame),@"bits_per_sample":@(format.mBitsPerChannel),@"frames":@(packets),@"format_flags":@(format.mFormatFlags)};
}
} // namespace
bool headlessNativeExportInFlight()noexcept{return inFlight.load(std::memory_order_acquire);}
void pollHeadlessNativeExport(bool currentARAReady)noexcept{
    if(!pthread_main_np()||!nativeApplicationAdmitted()||headlessNativeExportInFlight())return;
    auto& s=state();if(s.attempted)return;
    try{
        const auto& app=nativeApplication();
        if(!s.inspected){s.inspected=true;s.root=text(app.runDirectory);const std::string root=canonical(app.runDirectory.c_str());require(root==app.runDirectory,"Owned export run directory differs");
            NSString* path=[s.root stringByAppendingPathComponent:@"headless-export-request.json"];struct stat st{};if(lstat(path.fileSystemRepresentation,&st)){require(errno==ENOENT,"Cannot inspect owned export request");return;}s.requested=true;
            id request=[NSJSONSerialization JSONObjectWithData:readRequest(path) options:0 error:nil];require([request isKindOfClass:[NSDictionary class]]&&[request count]==5,"Invalid owned export request");s.request=request;
            require([request[@"schema"] isEqual:@"rysup.fl-ara-owned-export/1"]&&[request[@"run_id"] isEqual:text(app.runID)],"Owned export request identity differs");
            require(!getenv("RYSUP_FLARA_TEST_AUDIO_DEVICE_UID")&&std::string(getenv("RYSUP_FLARA_MODE")?:"")=="normal","Headless export requires normal physical-device mode");
            NSString* project=request[@"project"];NSString* output=request[@"output"];
            require([project isKindOfClass:[NSString class]]&&[output isKindOfClass:[NSString class]]&&[output isEqual:[s.root stringByAppendingPathComponent:@"export.wav"]],"Owned export paths differ");
            require([project isEqual:@(getenv("RYSUP_FLARA_PROJECT_PATH")?:"")]&&[project.pathExtension.lowercaseString isEqual:@"flp"]&&canonical(project.fileSystemRepresentation)==project.fileSystemRepresentation,"Owned export project differs");
            require([request[@"project_sha256"] isEqual:text(hashFile(project.fileSystemRepresentation))],"Owned export project bytes changed");
            receipt(@{@"state":@"waiting_for_current_ara_cache"});}
        if(!s.requested||!currentARAReady||!app.engineReady||currentNativeSaveTransaction())return;
        std::int32_t transport=-1;require(readActualNativeTransportState(transport)&&transport==0,"Native export requires actual stopped transport");
        require(getppid()==app.parentProcessID&&getpid()==app.processID,"Owned export process identity changed");verifyCode(app);
        NSString* output=s.request[@"output"];struct stat st{};require(lstat(output.fileSystemRepresentation,&st)&&errno==ENOENT,"Owned export output already exists");
        require([s.request[@"project_sha256"] isEqual:text(hashFile([s.request[@"project"] fileSystemRepresentation]))],"Owned export project changed before ready");
        const auto literal=retainedLiteral(output);s.attempted=true;bool expected=false;require(inFlight.compare_exchange_strong(expected,true,std::memory_order_acq_rel),"Native export already in flight");
        struct Clear{~Clear(){inFlight.store(false,std::memory_order_release);}} clear;
        receipt(@{@"state":@"native_export_in_flight"});writeNativeApplicationEvent("owned_headless_export_started");
        using Export=std::uint8_t(*)(std::uintptr_t,std::uint32_t,std::uintptr_t,std::uintptr_t);
        const auto result=reinterpret_cast<Export>(app.engine.imageBase+0x3211c0)(literal,0x21,0,0);
        require(getpid()==app.processID&&getppid()==app.parentProcessID&&readActualNativeTransportState(transport)&&transport==0,"Native export did not return to the owned stopped process");
        NSMutableDictionary* resultRow=[verifyOutput(output) mutableCopy];resultRow[@"state"]=@"native_returned_output_verified";resultRow[@"native_returned_success"]=@(result==1);resultRow[@"native_return_value"]=@(result);
        resultRow[@"completed"]=@(result==1);require(result==1,"Native export returned failure; output preserved");receipt(resultRow);writeNativeApplicationEvent("owned_headless_export_returned_output_verified");
    }catch(const std::exception& e){s.attempted=true;receipt(@{@"state":@"failed_preserved",@"completed":@NO,@"error":@(e.what())});writeNativeApplicationEvent("owned_headless_export_failed",e.what());}
    catch(...){s.attempted=true;receipt(@{@"state":@"failed_preserved",@"completed":@NO,@"error":@"Unknown native export failure"});}
}
} // namespace rysup::fl_ara
