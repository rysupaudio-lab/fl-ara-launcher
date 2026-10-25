#include "FLHeadlessNativeExport.h"
#include "FLNativeApplication.h"
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <limits.h>
namespace rysup::fl_ara { namespace {
struct State {bool inspected=false,requested=false;NSString* root=nil;NSDictionary* request=nil;};
State& state(){static State* s=new State;return *s;}
void require(bool okay,const char* error){if(!okay)throw std::runtime_error(error);}
std::string canonical(const char* p){char b[PATH_MAX];return p&&realpath(p,b)?b:"";}
NSString* text(const std::string& v){return [NSString stringWithUTF8String:v.c_str()];}
bool batchMode()noexcept{const char* value=getenv("RYSUP_FLARA_BATCH_EXPORT");return value&&!std::strcmp(value,"1");}
std::string hashFile(const char* path){
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);require(fd>=0,"Cannot read owned batch input");
    struct Close{int fd;~Close(){close(fd);}} closeFile{fd};struct stat a{},b{};
    require(!fstat(fd,&a)&&S_ISREG(a.st_mode)&&a.st_uid==getuid(),"Batch input is not an owned regular file");
    CC_SHA256_CTX h;CC_SHA256_Init(&h);std::array<unsigned char,65536> block{};ssize_t n;
    while((n=read(fd,block.data(),block.size()))>0)CC_SHA256_Update(&h,block.data(),static_cast<CC_LONG>(n));
    require(n==0&&!fstat(fd,&b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size&&a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec&&a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec,"Batch input changed while hashing");
    unsigned char digest[32];CC_SHA256_Final(digest,&h);std::string out;constexpr char hex[]="0123456789abcdef";
    for(auto v:digest){out+=hex[v>>4];out+=hex[v&15];}return out;
}
NSData* readRequest(NSString* path){
    int fd=open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);require(fd>=0,"Owned batch request unreadable");
    struct stat st{};if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)||st.st_size<2||st.st_size>16384){close(fd);throw std::runtime_error("Owned batch request has invalid type, permissions or size");}
    NSMutableData* data=[NSMutableData dataWithLength:static_cast<NSUInteger>(st.st_size)];size_t at=0;
    while(at<data.length){ssize_t n=read(fd,static_cast<char*>(data.mutableBytes)+at,data.length-at);if(n<=0){close(fd);throw std::runtime_error("Owned batch request short read");}at+=static_cast<size_t>(n);}close(fd);return data;
}
void receipt(NSDictionary* extra){
    auto& s=state();if(!s.root)return;const auto& app=nativeApplication();
    NSMutableDictionary* row=[@{@"schema":@"rysup.fl-ara-owned-export-result/1",@"route":@"normal_native_cli",@"run_id":text(app.runID),@"pid":@(getpid()),@"requested":@(s.requested),@"native_export_invoked_by_helper":@NO,@"native_audio_qualified":@NO,@"output_verified":@NO,@"automatic_save_or_quit_by_helper":@NO} mutableCopy];
    if(s.request[@"output"])row[@"output"]=s.request[@"output"];
    [row addEntriesFromDictionary:extra];NSData* data=[NSJSONSerialization dataWithJSONObject:row options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:nil];
    NSString* path=[s.root stringByAppendingPathComponent:@"headless-export-result.json"];
    if(data){[data writeToFile:path options:NSDataWritingAtomic error:nil];chmod(path.fileSystemRepresentation,0600);}
}
} // namespace
bool headlessNativeExportInFlight()noexcept{return false;}
void pollHeadlessNativeExport(bool)noexcept{
    if(!batchMode()||!pthread_main_np()||!nativeApplicationAdmitted())return;
    auto& s=state();if(s.inspected)return;s.inspected=true;
    try{
        const auto& app=nativeApplication();s.root=text(app.runDirectory);require(canonical(app.runDirectory.c_str())==app.runDirectory,"Owned batch run directory differs");
        NSString* path=[s.root stringByAppendingPathComponent:@"headless-export-request.json"];
        id request=[NSJSONSerialization JSONObjectWithData:readRequest(path) options:0 error:nil];require([request isKindOfClass:[NSDictionary class]]&&[request count]==5,"Invalid owned batch request");s.request=request;
        require([request[@"schema"] isEqual:@"rysup.fl-ara-owned-export/1"]&&[request[@"run_id"] isEqual:text(app.runID)],"Owned batch request identity differs");
        NSString* project=request[@"project"];NSString* output=request[@"output"];
        require([project isKindOfClass:[NSString class]]&&[output isKindOfClass:[NSString class]],"Owned batch paths missing");
        NSString* expected=[[s.root stringByAppendingPathComponent:@"export"] stringByAppendingPathComponent:[project.lastPathComponent.stringByDeletingPathExtension stringByAppendingPathExtension:@"wav"]];
        require([output isEqual:expected]&&[project isEqual:@(getenv("RYSUP_FLARA_PROJECT_PATH")?:"")]&&[project.pathExtension.lowercaseString isEqual:@"flp"]&&canonical(project.fileSystemRepresentation)==project.fileSystemRepresentation,"Owned batch paths differ");
        require([request[@"project_sha256"] isEqual:text(hashFile(project.fileSystemRepresentation))],"Owned batch input bytes changed");
        s.requested=true;receipt(@{@"state":@"native_cli_request_observed"});
    }catch(const std::exception& e){receipt(@{@"state":@"native_cli_request_observation_failed",@"error":@(e.what())});}
    catch(...){receipt(@{@"state":@"native_cli_request_observation_failed",@"error":@"Unknown request observation failure"});}
}
void observeHeadlessNativeBatchReturn(int nativeResult)noexcept{
    if(!batchMode()||!pthread_main_np()||!nativeApplicationAdmitted())return;
    pollHeadlessNativeExport(false);receipt(@{@"state":@"native_cli_creation_returned",@"native_creation_returned":@YES,@"native_create_result":@(nativeResult)});
}
} // namespace rysup::fl_ara
