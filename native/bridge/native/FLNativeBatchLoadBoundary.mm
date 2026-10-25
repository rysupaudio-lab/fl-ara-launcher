#include "FLNativeBatchLoadBoundary.h"
#include "FLNativeApplication.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>

extern "C" void rysup_fl_batch_after_load_thunk();
namespace rysup::fl_ara { namespace {
constexpr std::uintptr_t loader=0x31bf00,call=0x36fc8c,continuation=0x36fc90,cleanup=0x370064;
struct State {
    NativeBatchPrime prime=nullptr;void* context=nullptr;std::uintptr_t base=0;
    std::uintptr_t armedSlot=0,expectedSP=0,expectedFP=0;bool configured=false,priming=false;
};
State& state(){static State* value=new State;return *value;}
bool batchMode(){const char* v=getenv("RYSUP_FLARA_BATCH_EXPORT");return v&&!std::strcmp(v,"1");}
template<class T>bool readAt(std::uintptr_t p,T& value){mach_vm_size_t n=0;
    return p&&mach_vm_read_overwrite(mach_task_self(),p,sizeof(T),reinterpret_cast<mach_vm_address_t>(&value),&n)==KERN_SUCCESS&&n==sizeof(T);}
bool sealedCode(std::uintptr_t base){
    struct Pin{std::uintptr_t offset;std::uint32_t word;};
    constexpr Pin pins[]={
        {loader,0xa9bf7bfd},{loader+4,0x910003fd},
        {0x36fb20,0xa9bf7bfd},{0x36fb24,0x910003fd},{0x36fb28,0xa9bf53f3},
        {0x36fb2c,0xa9bf5bf5},{0x36fb30,0xa9bf5ff7},{0x36fb34,0xd10483ff},
        {call,0x97feb09d},{continuation,0xd000c180},
        {0x36fef8,0x1400005b},{cleanup,0x97f2c773},{0x3700bc,0xd65f03c0}};
    for(const auto& pin:pins){std::uint32_t word=0;if(!readAt(base+pin.offset,word)||word!=pin.word)return false;}
    return true;
}
bool writableStackData(std::uintptr_t slot){
    mach_vm_address_t address=slot;mach_vm_size_t size=0;vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
    const auto result=mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,reinterpret_cast<vm_region_info_t>(&info),&count,&object);
    if(object!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),object);
    return result==KERN_SUCCESS&&slot>=address&&size>=8&&slot-address<=size-8
        &&(info.protection&VM_PROT_READ)&&(info.protection&VM_PROT_WRITE)&&!(info.protection&VM_PROT_EXECUTE);
}
} // namespace
bool configureNativeBatchLoadBoundary(NativeBatchPrime prime,void* context)noexcept{
    if(!batchMode())return true;
#if !defined(__arm64__) || defined(__arm64e__)
    (void)prime;(void)context;return false;
#else
    if(!pthread_main_np()||!nativeApplicationAdmitted()||!prime)return false;
    auto& s=state();const auto& app=nativeApplication();
    if(s.configured||s.armedSlot||app.engine.imageBase>std::numeric_limits<std::uintptr_t>::max()-0x3700c0
        ||!app.engine.imageBase||app.engine.verifiedArm64SliceSHA256!=rysup::ara_lab::kFLNativeModelArm64SHA256
        ||app.engine.exactLoadedImagePath!=app.enginePath||!sealedCode(app.engine.imageBase))return false;
    s.prime=prime;s.context=context;s.base=app.engine.imageBase;s.configured=true;return true;
#endif
}
__attribute__((noinline)) bool armNativeBatchLoadBoundaryAtCurrentCallback()noexcept{
    if(!batchMode()||!pthread_main_np()||!nativeApplicationAdmitted())return false;
    auto& s=state();if(!s.configured||s.priming||!s.prime||nativeApplication().engine.imageBase!=s.base)return false;
    const auto thread=pthread_self();const auto high=reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(thread));
    const auto size=pthread_get_stacksize_np(thread);if(!high||size<4096||size>1024u*1024u*1024u||size>high)return false;
    const auto low=high-size;auto fp=reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    std::uintptr_t foundSlot=0,foundSP=0,foundFP=0;bool ended=false,ownedSeen=false;unsigned found=0;
    const auto thunk=reinterpret_cast<std::uintptr_t>(&rysup_fl_batch_after_load_thunk);
    for(unsigned n=0;n<256;++n){
        if((fp&15)||fp<low||fp>high-16)return false;
        struct Pair{std::uintptr_t previous,lr;} pair{};if(!readAt(fp,pair))return false;
        if(pair.lr==s.base+continuation){
            // The sealed batch executor's pushes +0x120 frame leave its SP
            // exactly0x150 below its FP. Loader's first saved pair is16 lower.
            if(pair.previous<=fp||pair.previous-fp!=0x160)return false;
            ++found;foundSlot=fp+8;foundSP=fp+16;foundFP=pair.previous;
        }
        if(s.armedSlot==fp+8&&pair.lr==thunk)ownedSeen=true;
        if(!pair.previous){ended=true;break;}
        if(pair.previous<=fp||pair.previous>high-16)return false;fp=pair.previous;
    }
    if(!ended)return false;
    if(ownedSeen)return found==0;
    // A native nonlocal unwind may abandon the prior loader frame. Its memory
    // is never restored or dereferenced through old ownership metadata.
    s.armedSlot=0;s.expectedSP=0;s.expectedFP=0;
    if(found!=1||!writableStackData(foundSlot)||!sealedCode(s.base))return false;
    s.armedSlot=foundSlot;s.expectedSP=foundSP;s.expectedFP=foundFP;
    auto expected=s.base+continuation;
    if(!__atomic_compare_exchange_n(reinterpret_cast<std::uintptr_t*>(foundSlot),&expected,thunk,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        s.armedSlot=0;s.expectedSP=0;s.expectedFP=0;return false;}
    writeNativeApplicationEvent("native_batch_load_return_armed");return true;
}
void finishNativeBatchLoadBoundary()noexcept{
    if(!pthread_main_np())return;auto& s=state();s.armedSlot=0;s.expectedSP=0;s.expectedFP=0;s.configured=false;s.prime=nullptr;s.context=nullptr;
}
// Reached only AFTER native loader restored its own frame and returned. The
// assembly context has already consumed/overwritten that old stack storage.
// Never write the old LR slot here. Return a native continuation, not a result
// to the loader. No exception is propagated across this custom return edge.
extern "C" std::uintptr_t rysup_fl_batch_after_load_dispatch(std::uintptr_t originalSP,std::uintptr_t originalFP)noexcept{
    auto& s=state();const auto target=s.base+cleanup;
    const bool owned=s.configured&&s.armedSlot&&originalSP==s.expectedSP&&originalFP==s.expectedFP;
    s.armedSlot=0;s.expectedSP=0;s.expectedFP=0;
    if(!owned||!pthread_main_np()||!nativeApplicationAdmitted()||!batchMode()||!s.prime||s.priming){
        writeNativeApplicationEvent("native_batch_load_return_identity_refused");return target;}
    s.priming=true;writeNativeApplicationEvent("native_batch_load_return_entered");
    const bool ready=s.prime(s.context);s.priming=false;
    writeNativeApplicationEvent(ready?"native_batch_prime_ready":"native_batch_prime_refused");
    //370064 is the native post-load bad-output cleanup path (36fef8). It
    // skips rendering and native completion; original stack locals are valid.
    return s.base+(ready?continuation:cleanup);
}
} // namespace rysup::fl_ara

#if defined(__arm64__) && !defined(__arm64e__)
// Existing signed helper text, never generated/relocated code or native text.
// Save all data GPRs and full SIMD/control state across the own C++ callback.
// x30 intentionally becomes the selected native return continuation.
extern "C" __attribute__((naked,noinline,used)) void rysup_fl_batch_after_load_thunk(){
    __asm__ volatile(
        "sub sp, sp, #0x320\n"
        "stp x0, x1, [sp, #0x0]\n"
        "stp x2, x3, [sp, #0x10]\n"
        "stp x4, x5, [sp, #0x20]\n"
        "stp x6, x7, [sp, #0x30]\n"
        "stp x8, x9, [sp, #0x40]\n"
        "stp x10, x11, [sp, #0x50]\n"
        "stp x12, x13, [sp, #0x60]\n"
        "stp x14, x15, [sp, #0x70]\n"
        "stp x16, x17, [sp, #0x80]\n"
        "stp x18, x19, [sp, #0x90]\n"
        "stp x20, x21, [sp, #0xa0]\n"
        "stp x22, x23, [sp, #0xb0]\n"
        "stp x24, x25, [sp, #0xc0]\n"
        "stp x26, x27, [sp, #0xd0]\n"
        "stp x28, x29, [sp, #0xe0]\n"
        "str x30, [sp, #0xf0]\n"
        "stp q0, q1, [sp, #0x100]\n"
        "stp q2, q3, [sp, #0x120]\n"
        "stp q4, q5, [sp, #0x140]\n"
        "stp q6, q7, [sp, #0x160]\n"
        "stp q8, q9, [sp, #0x180]\n"
        "stp q10, q11, [sp, #0x1a0]\n"
        "stp q12, q13, [sp, #0x1c0]\n"
        "stp q14, q15, [sp, #0x1e0]\n"
        "stp q16, q17, [sp, #0x200]\n"
        "stp q18, q19, [sp, #0x220]\n"
        "stp q20, q21, [sp, #0x240]\n"
        "stp q22, q23, [sp, #0x260]\n"
        "stp q24, q25, [sp, #0x280]\n"
        "stp q26, q27, [sp, #0x2a0]\n"
        "stp q28, q29, [sp, #0x2c0]\n"
        "stp q30, q31, [sp, #0x2e0]\n"
        "mrs x16, nzcv\n"
        "str x16, [sp, #0x300]\n"
        "mrs x16, fpcr\n"
        "str x16, [sp, #0x308]\n"
        "mrs x16, fpsr\n"
        "str x16, [sp, #0x310]\n"
        "add x0, sp, #0x320\n"
        "mov x1, x29\n"
        "bl _rysup_fl_batch_after_load_dispatch\n"
        "str x0, [sp, #0x318]\n"
        "ldr x16, [sp, #0x308]\n"
        "msr fpcr, x16\n"
        "ldr x16, [sp, #0x310]\n"
        "msr fpsr, x16\n"
        "ldr x16, [sp, #0x300]\n"
        "msr nzcv, x16\n"
        "ldp q0, q1, [sp, #0x100]\n"
        "ldp q2, q3, [sp, #0x120]\n"
        "ldp q4, q5, [sp, #0x140]\n"
        "ldp q6, q7, [sp, #0x160]\n"
        "ldp q8, q9, [sp, #0x180]\n"
        "ldp q10, q11, [sp, #0x1a0]\n"
        "ldp q12, q13, [sp, #0x1c0]\n"
        "ldp q14, q15, [sp, #0x1e0]\n"
        "ldp q16, q17, [sp, #0x200]\n"
        "ldp q18, q19, [sp, #0x220]\n"
        "ldp q20, q21, [sp, #0x240]\n"
        "ldp q22, q23, [sp, #0x260]\n"
        "ldp q24, q25, [sp, #0x280]\n"
        "ldp q26, q27, [sp, #0x2a0]\n"
        "ldp q28, q29, [sp, #0x2c0]\n"
        "ldp q30, q31, [sp, #0x2e0]\n"
        "ldp x0, x1, [sp, #0x0]\n"
        "ldp x2, x3, [sp, #0x10]\n"
        "ldp x4, x5, [sp, #0x20]\n"
        "ldp x6, x7, [sp, #0x30]\n"
        "ldp x8, x9, [sp, #0x40]\n"
        "ldp x10, x11, [sp, #0x50]\n"
        "ldp x12, x13, [sp, #0x60]\n"
        "ldp x14, x15, [sp, #0x70]\n"
        "ldp x16, x17, [sp, #0x80]\n"
        "ldp x18, x19, [sp, #0x90]\n"
        "ldp x20, x21, [sp, #0xa0]\n"
        "ldp x22, x23, [sp, #0xb0]\n"
        "ldp x24, x25, [sp, #0xc0]\n"
        "ldp x26, x27, [sp, #0xd0]\n"
        "ldp x28, x29, [sp, #0xe0]\n"
        "ldr x30, [sp, #0x318]\n"
        "add sp, sp, #0x320\n"
        "ret\n"
    );
}
#else
extern "C" void rysup_fl_batch_after_load_thunk(){std::abort();}
#endif
