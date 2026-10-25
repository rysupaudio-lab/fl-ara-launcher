#include "FLPointerProvenance.h"
#include <cassert>
#include <dlfcn.h>
#include <iostream>
static void fixture(){}
static_assert(__has_feature(ptrauth_intrinsics));
static_assert(!__has_feature(ptrauth_calls)); // intrinsic availability does not change the calling ABI
int main(){
    const auto raw=reinterpret_cast<std::uintptr_t>(&fixture);
    assert(rysup::fl_ara::pointer_provenance::codeAddress(raw)==raw);
    for(unsigned discriminator:{0U,1U,12345U,65535U}){
        void* signedPointer=ptrauth_sign_unauthenticated(reinterpret_cast<void*>(raw),ptrauth_key_function_pointer,discriminator);
        const auto value=reinterpret_cast<std::uintptr_t>(signedPointer);
        assert(rysup::fl_ara::pointer_provenance::codeAddress(value)==raw);
        Dl_info a{},b{};assert(dladdr(reinterpret_cast<void*>(raw),&a));assert(dladdr(reinterpret_cast<void*>(rysup::fl_ara::pointer_provenance::codeAddress(value)),&b));
        assert(a.dli_fbase==b.dli_fbase);
    }
    assert(rysup::fl_ara::pointer_provenance::codeAddress(0)==0);
    std::cout<<"PASS 10 pointer-provenance controls; signed tokens compared only, never called; no audio APIs\n";
}
