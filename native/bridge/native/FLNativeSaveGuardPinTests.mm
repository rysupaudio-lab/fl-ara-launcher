#include "FLNativeSaveGuard.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#if !defined(RYSUP_FL_SAVE_GUARD_TESTING)
#error This own-memory test requires RYSUP_FL_SAVE_GUARD_TESTING.
#endif

// Input is 35 offset/u32 records independently read from the hash-bound shipped
// ARM64 artifact. No image is loaded/executed; all tested bytes are own memory.
int main(int argc,char** argv){
    unsigned assertions=0;
    const auto check=[&](bool value){++assertions;if(!value){std::fprintf(stderr,"FAIL pin assertion %u\n",assertions);return false;}return true;};
    if(!check(argc==2))return 1;
    std::ifstream input(argv[1],std::ios::binary);
    struct Pin{std::uint64_t offset;std::uint32_t instruction;};
    std::vector<Pin> pins;
    while(input){Pin pin{};input.read(reinterpret_cast<char*>(&pin.offset),8);if(!input.gcount())break;
        if(!check(input.gcount()==8))return 1;
        input.read(reinterpret_cast<char*>(&pin.instruction),4);if(!check(input.gcount()==4))return 1;
        if(!check(pin.offset<=0xa63ed8))return 1;pins.push_back(pin);
    }
    if(!check(pins.size()==35))return 1;
    constexpr std::size_t size=0xa64000;
    auto* bytes=static_cast<unsigned char*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0));
    if(!check(bytes!=MAP_FAILED))return 1;
    const auto base=reinterpret_cast<std::uintptr_t>(bytes);
    using rysup::fl_ara::save_guard_testing::instructionPinsMatch;
    if(!check(!instructionPinsMatch(0))||!check(!instructionPinsMatch(std::numeric_limits<std::uintptr_t>::max()))
        ||!check(!instructionPinsMatch(base)))return 1;
    for(const auto& pin:pins)std::memcpy(bytes+pin.offset,&pin.instruction,4);
    if(!check(instructionPinsMatch(base)))return 1;
    for(const auto& pin:pins){
        bytes[pin.offset]^=1;
        if(!check(!instructionPinsMatch(base)))return 1;
        bytes[pin.offset]^=1;
        if(!check(instructionPinsMatch(base)))return 1;
    }
    if(!check(munmap(bytes,size)==0)||!check(!instructionPinsMatch(base)))return 1;
    std::printf("PASS %u instruction-pin assertions; all 35 independently artifact-read pins required; no engine instance or DAW run\n",assertions);
    return 0;
}
