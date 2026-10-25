#include "FLNativeSaveGuard.h"
#if !defined(RYSUP_FL_SAVE_GUARD_TESTING)
#include "FLNativeApplication.h"
#endif
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include <limits.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

namespace rysup::fl_ara { namespace {
constexpr unsigned failedBit=1,commitBit=2,finishedBit=4;
struct Node {NativeSaveTransaction view;std::atomic<unsigned> phase{0};bool finished=false;};
// Constant initialized: the RT entry must not enter State's lazy initializer.
std::atomic<Node*> activeNode{nullptr};
static_assert(std::atomic<Node*>::is_always_lock_free&&std::atomic<unsigned>::is_always_lock_free,
              "RT invalidation requires lock-free atomics");
using Move=BOOL(*)(id,SEL,NSString*,NSString*,NSError**);
struct State {
    NativeSaveGuardCallbacks callbacks;std::string enginePath;
    std::atomic<const NativeSaveGuardCallbacks*> liveCallbacks{nullptr};
    int pid=0;bool initialized=false;Class managerClass=Nil;SEL moveSelector=nullptr;Move originalMove=nullptr;
    std::atomic<bool> liveARA{false};std::atomic<std::size_t> pending{0};
    std::map<std::uintptr_t,Node*> transactions;
    // RT invalidation may already hold a node pointer. Retire for process life,
    // never free/reuse it after the main thread releases the owner cookie.
    std::vector<std::unique_ptr<Node>> retired;std::uint64_t nextSerial=0;
#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
    bool testing=false;std::string testingRoot;std::unique_ptr<NativeSaveRequest> synthetic;
#endif
};
State& state(){static State* s=new State;return *s;}
thread_local unsigned ownerCallbackDepth=0;
#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
thread_local bool syntheticCreatingRoute=false;
#endif
struct CallbackScope {CallbackScope(){++ownerCallbackDepth;}~CallbackScope(){--ownerCallbackDepth;}};
template<class T>bool readAt(std::uintptr_t p,T& out)noexcept{mach_vm_size_t n=0;return p&&mach_vm_read_overwrite(mach_task_self(),p,sizeof(T),(mach_vm_address_t)&out,&n)==KERN_SUCCESS&&n==sizeof(T);}
bool readBytes(std::uintptr_t p,void* out,std::size_t bytes)noexcept{mach_vm_size_t n=0;return p&&mach_vm_read_overwrite(mach_task_self(),p,bytes,(mach_vm_address_t)out,&n)==KERN_SUCCESS&&n==bytes;}
std::uintptr_t stripped(std::uintptr_t p)noexcept{
#if __has_feature(ptrauth_calls)
    return (std::uintptr_t)ptrauth_strip((void*)p,ptrauth_key_return_address);
#else
    return p;
#endif
}
bool ownerNeedsProtection()noexcept{
    auto& s=state();
    // Read the owner at the actual boundary. A stale main-thread false snapshot
    // must not overwrite a background observation of a genuine ARA component or
    // protected envelope. The callback itself is atomic-only and immutable.
    const auto* callbacks=s.liveCallbacks.load(std::memory_order_acquire);
    return callbacks&&callbacks->hasLiveARA?callbacks->hasLiveARA(callbacks->context)
        :s.liveARA.load(std::memory_order_acquire);
}
bool needed()noexcept{return state().pending.load(std::memory_order_acquire)!=0||ownerNeedsProtection();}
struct Frame {std::uintptr_t key=0,engine=0;bool createRoute=false,destructiveRoute=false,sourceSealed=false;};
struct Frames {std::array<Frame,16> values{};std::size_t count=0;bool walkValid=true;};
bool saveCodeSealed(std::uintptr_t base)noexcept{
    // Read-only instruction identity pins derived from the hash-bound ARM64
    // evidence. NativeApplication separately hashes the complete loaded file.
    struct Pin{std::uintptr_t offset;std::uint32_t instruction;};
    constexpr Pin pins[]={
        {0x419bb0,0xa9bf7bfd},{0x419bb4,0x910003fd},{0x419bb8,0xd10a83ff},
        {0x419bbc,0xf90003e0},{0x419bc0,0xb9000be1},{0x419bc4,0xf9000be2},
        {0x419bc8,0x390063e3},{0x419c70,0x94192810},{0x419d30,0x97fc1a74},
        {0x419d60,0x97f2c3f0},{0xa12960,0xa9bf7bfd},{0xa12964,0x910003fd},
        {0xa12968,0xd10043ff},{0xa1296c,0xb90003e2},{0xa12970,0x941cc1bc},
        {0xa63d48,0x94000032},{0xa63ed8,0x97ff11de},{0xa2871c,0x97db3701},
        {0xf6380,0x97ffffbc},{0xf62c8,0x942471a6},
        {0xf6270,0xa9bf7bfd},{0xf6274,0x910003fd},
        {0xf6320,0xa9bf7bfd},{0xf6324,0x910003fd},
        {0xa28650,0xa9bf7bfd},{0xa28654,0x910003fd},
        {0xa63e10,0xa9bf7bfd},{0xa63e14,0x910003fd},
        {0xa63cb0,0xa9bf7bfd},{0xa63cb4,0x910003fd},
        {0xf62bc,0x53003e82},{0xf62c4,0x5280c041},
        {0xa28704,0x12181c00},{0xa2870c,0x54000109},{0x419c60,0x529fe003}};
    for(const auto& pin:pins){std::uint32_t actual=0;if(!readAt(base+pin.offset,actual)||actual!=pin.instruction)return false;}
    return true;
}
bool isEnginePC(std::uintptr_t pc,std::uintptr_t& base)noexcept{
    auto& s=state();Dl_info image{};char path[PATH_MAX];
    if(!pc||s.enginePath.empty()||!dladdr((void*)pc,&image)||!image.dli_fname||!realpath(image.dli_fname,path)||s.enginePath!=path)return false;
    base=(std::uintptr_t)image.dli_fbase;return pc>=base;
}
bool loadedSaveCodeReady()noexcept{
#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
    return state().testing;
#else
    // Code-stage admission deliberately precedes CreateFruityInstance. No
    // engine object, transport global, source graph or native lock is needed.
    const auto& app=nativeApplication();const auto& binding=app.engine;
    if(!pthread_main_np()||!nativeApplicationAdmitted()||!binding.imageBase
        ||app.enginePath.empty()||state().enginePath!=app.enginePath
        ||binding.exactLoadedImagePath!=app.enginePath
        ||binding.verifiedArm64SliceSHA256!=rysup::ara_lab::kFLNativeModelArm64SHA256)return false;
    if(binding.imageBase>UINTPTR_MAX-0xa63edc)return false;
    std::uintptr_t actualBase=0;
    return isEnginePC(binding.imageBase+0x419bb0,actualBase)
        &&actualBase==binding.imageBase&&saveCodeSealed(binding.imageBase);
#endif
}
__attribute__((noinline)) Frames observeFrames()noexcept{
    Frames frames;
#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
    auto& test=state();if(test.testing){if(test.synthetic){frames.values[0]={test.synthetic->actualSaveFrame,0,syntheticCreatingRoute,false,true};frames.count=1;}return frames;}
#endif
    const auto thread=pthread_self();const auto upper=(std::uintptr_t)pthread_get_stackaddr_np(thread);
    const auto size=pthread_get_stacksize_np(thread);if(!upper||size<4096||size>1024u*1024u*1024u){frames.walkValid=false;return frames;}
    const auto lower=upper-size;auto fp=(std::uintptr_t)__builtin_frame_address(0);
    std::array<std::uintptr_t,128> allReturns{};std::size_t returns=0;bool ended=false;
    for(unsigned depth=0;depth<128;++depth){
        if((fp&15)||fp<lower||fp>upper-16){frames.walkValid=false;break;}
        struct Pair{std::uintptr_t previous,returnPC;} pair{};if(!readAt(fp,pair)){frames.walkValid=false;break;}
        pair.previous=stripped(pair.previous);pair.returnPC=stripped(pair.returnPC);
        std::uintptr_t engine=0;const bool enginePC=isEnginePC(pair.returnPC,engine);
        const auto offset=enginePC?pair.returnPC-engine:0;
        if(returns<allReturns.size())allReturns[returns++]=offset;
        if(enginePC){
            if(offset>=0x419bb0&&offset<0x419e3c){
                if(pair.previous<=fp||pair.previous<lower+0x2a0||pair.previous>upper-16||frames.count==frames.values.size()){frames.walkValid=false;break;}
                // Exact creating chain. Non-engine frames remain sentinel
                // entries, so skipping a foreign/intervening frame cannot pass.
                constexpr std::uintptr_t chain[]={0xa12974,0xf62cc,0xf6384,0xa28720,0xa63edc,0xa63d4c,0x419c74};
                bool creating=returns>=7;
                if(creating)for(std::size_t i=0;i<7;++i)if(allReturns[returns-7+i]!=chain[i])creating=false;
                bool destructive=false;
                for(std::size_t i=0;i<returns;++i){
                    // 419d34 is the actual native replacement helper's return;
                    // 419d64 is the native error cleanup's temp-unlink return.
                    if(allReturns[i]==0x419d34||allReturns[i]==0x419d64)destructive=true;
                }
                std::uint32_t call=0;const bool linked=readAt(pair.returnPC-4,call)
                    &&((call&0xfc000000)==0x94000000||(call&0xfffffc1f)==0xd63f0000);
                frames.values[frames.count++]={pair.previous,engine,creating,destructive,linked&&saveCodeSealed(engine)};
            }
        }
        if(!pair.previous){ended=true;break;}
        if(pair.previous<=fp||pair.previous>upper-16){frames.walkValid=false;break;}fp=pair.previous;
    }
    if(!ended)frames.walkValid=false;
    return frames;
}
bool contains(const Frames& f,std::uintptr_t key)noexcept{for(std::size_t i=0;i<f.count;++i)if(f.values[i].key==key)return true;return false;}
bool unicodePath(std::uintptr_t data,std::string& out){
    if(data<24||(data&1))return false;std::array<unsigned char,24> header{},again{};
    if(!readBytes(data-24,header.data(),24))return false;std::uint16_t page=0,width=0;std::int64_t length=0,refs=0;
    std::memcpy(&page,header.data(),2);std::memcpy(&width,header.data()+2,2);std::memcpy(&length,header.data()+16,8);
    std::memcpy(&refs,header.data()+8,8);
    // Exact sealed native Save-frame UnicodeString profile: C10 genuine Save
    // captured dynamic codePage0/element2; immutable literals use1200/element2.
    // Encoding remains UTF-16, with every other header/path/coherence check.
    if((page!=0&&page!=1200)||width!=2||length<=0||length>=PATH_MAX||refs< -1)return false;
    std::vector<std::uint16_t> chars(std::size_t(length)+1);if(!readBytes(data,chars.data(),chars.size()*2)||chars.back()!=0)return false;
    out.clear();out.reserve(std::size_t(length));
    for(std::size_t i=0;i<std::size_t(length);++i){std::uint32_t c=chars[i];if(!c)return false;
        if(c>=0xd800&&c<=0xdbff){if(++i>=std::size_t(length)||chars[i]<0xdc00||chars[i]>0xdfff)return false;c=0x10000+((c-0xd800)<<10)+(chars[i]-0xdc00);}
        else if(c>=0xdc00&&c<=0xdfff)return false;
        if(c<0x80)out+=char(c);else if(c<0x800){out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}
        else if(c<0x10000){out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
        else{out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    }
    std::vector<std::uint16_t> repeated(chars.size());
    return !out.empty()&&out[0]=='/'&&readBytes(data-24,again.data(),24)&&header==again
        &&readBytes(data,repeated.data(),repeated.size()*2)&&chars==repeated;
}
bool requestFromFrame(const Frame& f,NativeSaveRequest& r){
#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
    auto& test=state();if(test.testing&&test.synthetic){r=*test.synthetic;return r.actualSaveFrame==f.key;}
#endif
    if(!f.sourceSealed)return false;
    r.actualSaveFrame=f.key;if(f.key<0x2a0)return false;const auto locals=f.key-0x2a0;std::uintptr_t target=0,opened=0;
    if(!readAt(locals,target)||!readAt(locals+0x30,opened)||!readAt(locals+0x18,r.nativeFlags)||!readAt(locals+8,r.actualSaveType))return false;
    r.directedTarget=(r.nativeFlags&1)!=0;
    if(!unicodePath(target,r.targetPath)||!unicodePath(opened,r.actualOpenPath)
        ||!(r.directedTarget?r.actualOpenPath==r.targetPath:r.actualOpenPath==r.targetPath+"~"))return false;
    std::uintptr_t targetAgain=0,openedAgain=0;std::uint8_t flagsAgain=0;std::uint32_t typeAgain=0;
    return readAt(locals,targetAgain)&&readAt(locals+0x30,openedAgain)&&readAt(locals+0x18,flagsAgain)
        &&readAt(locals+8,typeAgain)&&target==targetAgain&&opened==openedAgain&&r.nativeFlags==flagsAgain&&r.actualSaveType==typeAgain;
}
void sync(Node& n)noexcept{const auto p=n.phase.load(std::memory_order_acquire);n.view.failed=(p&failedBit)!=0;n.view.commitBegun=(p&commitBit)!=0;if(n.view.failed&&n.view.rejection==NativeSaveRejection::None)n.view.rejection=NativeSaveRejection::LateSerializationFailure;}
bool failNode(Node& n,NativeSaveRejection why)noexcept{
    auto phase=n.phase.load(std::memory_order_acquire);
    while(!(phase&(commitBit|finishedBit))){if(n.phase.compare_exchange_weak(phase,phase|failedBit,std::memory_order_acq_rel)){n.view.rejection=why;sync(n);return true;}}
    return false;
}
void rejected(const NativeSaveRequest& r,NativeSaveRejection why)noexcept{auto& s=state();if(s.callbacks.rejected){CallbackScope scope;s.callbacks.rejected(r,why,s.callbacks.context);}}
Node* nodeFor(std::uintptr_t key)noexcept{auto& s=state();const auto it=s.transactions.find(key);return it==s.transactions.end()?nullptr:it->second;}
Node& newNode(const NativeSaveRequest& r){auto& s=state();auto p=std::make_unique<Node>();p->view.request=r;p->view.serial=++s.nextSerial;auto* node=p.get();s.retired.push_back(std::move(p));s.transactions.emplace(r.actualSaveFrame,node);s.pending.store(s.transactions.size(),std::memory_order_release);return *node;}
BOOL observedMove(id,SEL,NSString*,NSString*,NSError**);
bool moveGuardEffective()noexcept{auto& s=state();if(!s.initialized||!s.managerClass||!s.moveSelector||!s.originalMove)return false;id manager=[NSFileManager defaultManager];Method method=class_getInstanceMethod(object_getClass(manager),s.moveSelector);return object_getClass(manager)==s.managerClass&&method&&method_getImplementation(method)==(IMP)&observedMove;}
bool installMoveGuard()noexcept{auto& s=state();id manager=[NSFileManager defaultManager];s.managerClass=object_getClass(manager);s.moveSelector=@selector(moveItemAtPath:toPath:error:);Method method=class_getInstanceMethod(s.managerClass,s.moveSelector);if(!method)return false;s.originalMove=(Move)method_getImplementation(method);if(!s.originalMove||s.originalMove==(Move)&observedMove)return false;
    const char* types=method_getTypeEncoding(method);
    if(!class_addMethod(s.managerClass,s.moveSelector,(IMP)&observedMove,types))method_setImplementation(method,(IMP)&observedMove);
    s.initialized=true;return moveGuardEffective();
}
bool admitted()noexcept{
    auto& s=state();if(!s.initialized||s.pid!=getpid())return false;
#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
    if(s.testing)return true;
#endif
#if !defined(RYSUP_FL_SAVE_GUARD_TESTING)
    return nativeApplicationAdmitted();
#else
    return false;
#endif
}
bool stopped()noexcept{
#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
    if(state().testing)return true;
#endif
#if !defined(RYSUP_FL_SAVE_GUARD_TESTING)
    std::int32_t current=-1;return readActualNativeTransportState(current)&&current==0;
#else
    return false;
#endif
}
bool allowWriteOpen(const char* path,int flags){
    if(!needed()||!(flags&(O_CREAT|O_TRUNC|O_WRONLY|O_RDWR)))return true;
    const auto frames=observeFrames();if(!frames.count)return frames.walkValid;const auto& frame=frames.values[0];
    // No map, owner callback or mutable transaction access from an off-main
    // native Save path. Such a path is unsupported and must fail before create.
    if(!pthread_main_np())return false;
    NativeSaveRequest r;if(!requestFromFrame(frame,r)){r.actualSaveFrame=frame.key;auto* n=nodeFor(frame.key);if(!n)n=&newNode(r);failNode(*n,NativeSaveRejection::NativeFrameUnreadable);rejected(r,NativeSaveRejection::NativeFrameUnreadable);return false;}
    // Other write paths reached while native Save is on the stack are unrelated
    // unless they are the actual target selected by this precise native frame.
    if(!path||r.actualOpenPath!=path){
        // The exact native constructor chain is stronger authority than path
        // spelling. Never forward its create because conversion was unexpected.
        if(!frame.createRoute)return true;
        auto* n=nodeFor(frame.key);if(!n)n=&newNode(r);
        failNode(*n,NativeSaveRejection::NativeOpenRouteMismatch);
        rejected(r,NativeSaveRejection::NativeOpenRouteMismatch);return false;
    }r.openFlags=flags;
    if(auto* old=nodeFor(frame.key)){failNode(*old,NativeSaveRejection::NestedOrRepeatedOpen);rejected(r,NativeSaveRejection::NestedOrRepeatedOpen);return false;}
    auto& n=newNode(r);n.view.openAttempted=true;NativeSaveRejection why=NativeSaveRejection::None;
    if(!pthread_main_np())why=NativeSaveRejection::NotMainThread;
    else if(!admitted()||!frames.walkValid||!frame.createRoute||flags!=(O_RDWR|O_CREAT|O_TRUNC))why=NativeSaveRejection::NativeOpenRouteMismatch;
    else if(r.directedTarget)why=NativeSaveRejection::DirectTargetUnsupported;
    else if(r.actualSaveType!=0)why=NativeSaveRejection::UnsupportedSaveType;
    else if(state().transactions.size()!=1)why=NativeSaveRejection::NestedOrRepeatedOpen;
    else if(ownerCallbackDepth)why=NativeSaveRejection::ReentrantPreflight;
    else if(!moveGuardEffective())why=NativeSaveRejection::MoveGuardUnavailable;
    else if(!stopped())why=NativeSaveRejection::NativeTransportNotStopped;
    if(why==NativeSaveRejection::None){activeNode.store(&n,std::memory_order_release);bool ready=false;
        if(state().callbacks.preflight){CallbackScope scope;ready=state().callbacks.preflight(r,&n.view.ownerCookie,state().callbacks.context);}
        if(!ready)why=NativeSaveRejection::OwnerNotReady;
    }
    if(why!=NativeSaveRejection::None){failNode(n,why);rejected(r,why);return false;}
    sync(n);if(n.view.failed){rejected(r,NativeSaveRejection::LateSerializationFailure);return false;}
    n.view.openAllowed=true;return true;
}
bool matchesProjectPath(const NativeSaveRequest& request,const char* path)noexcept{
    return path&&(request.targetPath==path||request.actualOpenPath==path);
}
bool allowDestructiveEffect(const char* first,const char* second=nullptr){
    const bool required=needed();const auto* callbacks=state().liveCallbacks.load(std::memory_order_acquire);
    // Predicate-backed ownership may appear during a native Save. Observe its
    // first real commit even while false; legacy no-owner controls still pass.
    if(!required&&!(callbacks&&callbacks->hasLiveARA))return true;
    const auto frames=observeFrames();if(!frames.count)return !required||frames.walkValid;
    if(!pthread_main_np())return false;
    auto* n=nodeFor(frames.values[0].key);
    // Backup moves have the real target as source; final moves have the temp
    // as source / target as destination. Cleanup unlinks name target or temp.
    // A different path while Save happens to be on the stack is forwarded.
    NativeSaveRequest nativeRequest;
    const auto* request=n?&n->view.request:&nativeRequest;
    if(!n&&!requestFromFrame(frames.values[0],nativeRequest))return false;
    if(!frames.values[0].destructiveRoute&&!matchesProjectPath(*request,first)&&!matchesProjectPath(*request,second))return true;
    if(!n){
        if(required){rejected(nativeRequest,NativeSaveRejection::OwnerNotReady);return false;}
        if(!admitted()||!frames.walkValid||!frames.values[0].sourceSealed
            ||nativeRequest.directedTarget||nativeRequest.actualSaveType!=0){rejected(nativeRequest,NativeSaveRejection::NativeFrameUnreadable);return false;}
        // No ARA freeze/state getter is appropriate here: this is the native
        // ordinary Save already at its first destructive effect. Publish before
        // CAS so concurrent genuine-ARA discovery can invalidate before commit.
        n=&newNode(nativeRequest);n->view.ordinaryCommit=true;n->view.openAllowed=true;
        activeNode.store(n,std::memory_order_release);
        if(ownerNeedsProtection()){
            failNode(*n,NativeSaveRejection::OwnerNotReady);
            rejected(n->view.request,NativeSaveRejection::OwnerNotReady);return false;
        }
        unsigned expected=0;
        if(!n->phase.compare_exchange_strong(expected,commitBit,std::memory_order_acq_rel)){
            sync(*n);rejected(n->view.request,NativeSaveRejection::LateSerializationFailure);return false;
        }
        sync(*n);return true;
    }
    sync(*n);if(n->view.commitBegun)return true;
    if(n->view.failed||!n->view.openAllowed||!pthread_main_np()||!admitted()||!frames.walkValid||!frames.values[0].sourceSealed){rejected(n->view.request,n->view.rejection==NativeSaveRejection::None?NativeSaveRejection::NativeFrameUnreadable:n->view.rejection);return false;}
    bool approved=false;if(state().callbacks.precommit){CallbackScope scope;approved=state().callbacks.precommit(n->view.request,n->view.ownerCookie,state().callbacks.context);}
    if(!approved){failNode(*n,NativeSaveRejection::OwnerPrecommitRefused);rejected(n->view.request,NativeSaveRejection::OwnerPrecommitRefused);return false;}
    unsigned expected=0;
    // Linearization point: RT invalidation wins and vetoes BEFORE any effect,
    // or commit wins and later dirtiness cannot strand an already-moved target.
    if(!n->phase.compare_exchange_strong(expected,commitBit,std::memory_order_acq_rel)){sync(*n);rejected(n->view.request,NativeSaveRejection::LateSerializationFailure);return false;}
    sync(*n);return true;
}
__attribute__((noinline)) int observedOpen(const char* path,int flags,...){
    mode_t mode=0;if(flags&O_CREAT){va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
    bool allow=false;try{allow=allowWriteOpen(path,flags);}catch(...){allow=false;}
    if(!allow){errno=EBUSY;return -1;}
    return (flags&O_CREAT) ? ::open(path,flags,mode) : ::open(path,flags);
}
__attribute__((noinline)) int observedUnlink(const char* path){bool allow=false;try{allow=allowDestructiveEffect(path);}catch(...){allow=false;}if(!allow){errno=EBUSY;return -1;}return ::unlink(path);}
BOOL observedMove(id self,SEL selector,NSString* from,NSString* to,NSError** error){bool allow=false;try{allow=allowDestructiveEffect([from fileSystemRepresentation],[to fileSystemRepresentation]);}catch(...){allow=false;}
    if(!allow){if(error)*error=[NSError errorWithDomain:NSPOSIXErrorDomain code:EBUSY userInfo:@{NSLocalizedDescriptionKey:@"ARA project save is not ready; the previous project was preserved."}];errno=EBUSY;return NO;}
    return state().originalMove(self,selector,from,to,error);
}
} // namespace
bool initializeNativeSaveGuard(const NativeSaveGuardCallbacks& callbacks)noexcept{try{auto& s=state();if(s.initialized||!pthread_main_np())return false;
#if !defined(RYSUP_FL_SAVE_GUARD_TESTING)
    if(!nativeApplicationAdmitted())return false;s.enginePath=nativeApplication().enginePath;
    if(!loadedSaveCodeReady())return false;
#else
    return false;
#endif
    s.callbacks=callbacks;s.pid=getpid();const bool ready=installMoveGuard();
    if(ready)s.liveCallbacks.store(&s.callbacks,std::memory_order_release);return ready;
}catch(...){return false;}}
void setNativeSaveGuardHasLiveARA(bool value)noexcept{state().liveARA.store(value,std::memory_order_release);}
bool nativeSaveGuardReady()noexcept{return pthread_main_np()&&admitted()&&loadedSaveCodeReady()&&moveGuardEffective();}
void pollNativeSaveGuardTransactions()noexcept{if(!pthread_main_np())return;try{auto& s=state();const auto frames=observeFrames();if(!frames.walkValid)return;
    for(auto it=s.transactions.begin();it!=s.transactions.end();){auto& n=*it->second;if(contains(frames,it->first)){++it;continue;}
        n.phase.fetch_or(finishedBit,std::memory_order_acq_rel);sync(n);Node* expected=&n;activeNode.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel);
        if(!n.finished){n.finished=true;if(s.callbacks.finished){CallbackScope scope;s.callbacks.finished(n.view,s.callbacks.context);}n.view.ownerCookie=nullptr;}
        it=s.transactions.erase(it);
    }s.pending.store(s.transactions.size(),std::memory_order_release);
}catch(...){}}
const NativeSaveTransaction* currentNativeSaveTransaction()noexcept{if(!pthread_main_np())return nullptr;try{const auto frames=observeFrames();if(!frames.walkValid||!frames.count||!frames.values[0].sourceSealed)return nullptr;auto* n=nodeFor(frames.values[0].key);if(!n)return nullptr;sync(*n);return &n->view;}catch(...){return nullptr;}}
bool markCurrentNativeSaveFailed(NativeSaveRejection reason)noexcept{if(!pthread_main_np())return false;try{const auto frames=observeFrames();if(!frames.count)return false;auto* n=nodeFor(frames.values[0].key);if(!n)return false;return failNode(*n,reason);}catch(...){return false;}}
bool markActiveNativeSaveFailedFromAnyThread()noexcept{auto* n=activeNode.load(std::memory_order_acquire);if(!n)return false;auto phase=n->phase.load(std::memory_order_acquire);while(!(phase&(commitBit|finishedBit))){if(n->phase.compare_exchange_weak(phase,phase|failedBit,std::memory_order_acq_rel))return true;}return false;}
bool hasActiveNativeSaveFromAnyThread()noexcept{const auto* n=activeNode.load(std::memory_order_acquire);return n&&(n->phase.load(std::memory_order_acquire)&finishedBit)==0;}
const char* nativeSaveRejection(NativeSaveRejection r)noexcept{switch(r){
case NativeSaveRejection::None:return "none";case NativeSaveRejection::NotMainThread:return "not_main_thread";
case NativeSaveRejection::NativeFrameUnreadable:return "native_save_frame_unreadable";case NativeSaveRejection::NativeOpenRouteMismatch:return "native_create_route_mismatch";
case NativeSaveRejection::DirectTargetUnsupported:return "direct_target_save_unsupported";case NativeSaveRejection::NativeTransportNotStopped:return "native_transport_not_stopped";
case NativeSaveRejection::OwnerNotReady:return "prepared_ara_archive_not_ready";case NativeSaveRejection::NestedOrRepeatedOpen:return "nested_or_repeated_write_open";
case NativeSaveRejection::MoveGuardUnavailable:return "native_move_guard_unavailable";case NativeSaveRejection::LateSerializationFailure:return "native_serialization_failed";
case NativeSaveRejection::ReentrantPreflight:return "reentrant_save_preflight";case NativeSaveRejection::OwnerPrecommitRefused:return "native_precommit_refused";
case NativeSaveRejection::UnsupportedSaveType:return "unsupported_native_save_type";}return "unknown";}
#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
namespace save_guard_testing {
bool decodeSaveUnicodePath(std::uintptr_t data,std::string& output)noexcept{try{return unicodePath(data,output);}catch(...){return false;}}
bool instructionPinsMatch(std::uintptr_t base)noexcept{return base&&base<=UINTPTR_MAX-0xa63edc&&saveCodeSealed(base);}
bool initialize(const NativeSaveGuardCallbacks& callbacks,const std::string& root)noexcept{try{auto& s=state();char path[PATH_MAX];struct stat st{};if(s.initialized||!pthread_main_np()||!realpath(root.c_str(),path)||root!=path||lstat(path,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0022))return false;s.testing=true;s.testingRoot=root;s.callbacks=callbacks;s.pid=getpid();const bool ready=installMoveGuard();if(ready)s.liveCallbacks.store(&s.callbacks,std::memory_order_release);return ready;}catch(...){return false;}}
bool setFrame(const NativeSaveRequest& r)noexcept{try{auto& s=state();if(!s.testing||!pthread_main_np()||!r.actualSaveFrame||r.targetPath.find(s.testingRoot+"/")!=0||r.actualOpenPath.find(s.testingRoot+"/")!=0)return false;s.synthetic=std::make_unique<NativeSaveRequest>(r);return true;}catch(...){return false;}}
void clearFrame()noexcept{if(pthread_main_np())state().synthetic.reset();}
int open(const char* p,int flags,unsigned mode)noexcept{
    auto& s=state();syntheticCreatingRoute=s.synthetic&&p&&s.synthetic->actualOpenPath==p;
    const int result=(flags&O_CREAT)?observedOpen(p,flags,(int)mode):observedOpen(p,flags);
    syntheticCreatingRoute=false;return result;
}
int unlink(const char* p)noexcept{return observedUnlink(p);}
}
#endif
} // namespace rysup::fl_ara
struct FLNativeSaveInterpose {const void* replacement;const void* original;};
__attribute__((used,section("__DATA,__interpose")))static const FLNativeSaveInterpose nativeSaveInterposes[]={
    {(const void*)rysup::fl_ara::observedOpen,(const void*)open},
    {(const void*)rysup::fl_ara::observedUnlink,(const void*)unlink}
};
