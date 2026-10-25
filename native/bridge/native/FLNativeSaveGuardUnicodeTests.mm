#include "FLNativeSaveGuard.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(RYSUP_FL_SAVE_GUARD_TESTING)
#error Own-memory Unicode controls require RYSUP_FL_SAVE_GUARD_TESTING.
#endif

namespace {
unsigned checks=0;
bool check(bool value,const char* label){++checks;if(!value)std::fprintf(stderr,"FAIL %u: %s\n",checks,label);return value;}
#define CHECK(value) do{if(!check((value),#value))return 1;}while(false)
struct Fixture {
    // Own aligned data only. Never load/call a vendor image or touch files.
    std::vector<std::uint64_t> storage;
    explicit Fixture(const std::u16string& text):storage((24+(text.size()+1)*2+7)/8,0){
        std::uint16_t cp=0,width=2;std::int64_t refs=1,length=static_cast<std::int64_t>(text.size());
        set(0,cp);set(2,width);set(8,refs);set(16,length);
        std::memcpy(raw()+24,text.data(),text.size()*2);
    }
    unsigned char* raw(){return reinterpret_cast<unsigned char*>(storage.data());}
    template<class T>void set(std::size_t offset,T value){std::memcpy(raw()+offset,&value,sizeof value);}
    std::uintptr_t data(){return reinterpret_cast<std::uintptr_t>(raw()+24);}
    bool decode(std::string& out){return rysup::fl_ara::save_guard_testing::decodeSaveUnicodePath(data(),out);}
};
}
int main(){
    // Exact C10 observed header bytes: CP0/element2/ref7/length154. Substitute
    // an equally long owned test pathname, retaining no private path in code.
    constexpr std::array<unsigned char,24> capturedTarget={
        0,0,2,0,0,0,0,0,7,0,0,0,0,0,0,0,0x9a,0,0,0,0,0,0,0};
    constexpr std::array<unsigned char,24> capturedTemporary={
        0,0,2,0,0,0,0,0,1,0,0,0,0,0,0,0,0x9b,0,0,0,0,0,0,0};
    std::u16string target=u"/owned/";target.append(154-target.size(),u'a');
    Fixture a(target),b(target+u"~");std::memcpy(a.raw(),capturedTarget.data(),24);std::memcpy(b.raw(),capturedTemporary.data(),24);
    std::string decodedTarget,decodedTemporary;
    CHECK(a.decode(decodedTarget));CHECK(b.decode(decodedTemporary));
    CHECK(decodedTarget.size()==154);CHECK(decodedTemporary==decodedTarget+"~");
    CHECK(std::memcmp(a.raw(),capturedTarget.data(),24)==0);CHECK(std::memcmp(b.raw(),capturedTemporary.data(),24)==0);
    Fixture unicode(u"/owned/é-😀.flp");std::string output;
    CHECK(unicode.decode(output));CHECK(output=="/owned/\xc3\xa9-\xf0\x9f\x98\x80.flp");
    unicode.set(0,std::uint16_t(1200));CHECK(unicode.decode(output));
    unicode.set(8,std::int64_t(-1));CHECK(unicode.decode(output));
    for(const auto cp:{std::uint16_t(1252),std::uint16_t(65001),std::uint16_t(65535)}){
        Fixture bad(u"/owned/x.flp");bad.set(0,cp);CHECK(!bad.decode(output));
    }
    {Fixture bad(u"/owned/x.flp");bad.set(2,std::uint16_t(1));CHECK(!bad.decode(output));}
    {Fixture bad(u"/owned/x.flp");bad.set(2,std::uint16_t(4));CHECK(!bad.decode(output));}
    {Fixture bad(u"/owned/x.flp");bad.set(8,std::int64_t(-2));CHECK(!bad.decode(output));}
    {Fixture bad(u"/owned/x.flp");bad.set(16,std::int64_t(0));CHECK(!bad.decode(output));}
    {Fixture bad(u"/owned/x.flp");bad.set(16,std::int64_t(-1));CHECK(!bad.decode(output));}
    {Fixture bad(u"/owned/x.flp");bad.set(16,std::int64_t(1024));CHECK(!bad.decode(output));}
    {Fixture bad(u"/owned/x.flp");bad.set(24+12*2,std::uint16_t(1));CHECK(!bad.decode(output));}
    {std::u16string text=u"/owned/x.flp";text[3]=0;Fixture bad(text);CHECK(!bad.decode(output));}
    {std::u16string text=u"/owned/x.flp";text[3]=0xd800;Fixture bad(text);CHECK(!bad.decode(output));}
    {std::u16string text=u"/owned/x.flp";text[3]=0xdc00;Fixture bad(text);CHECK(!bad.decode(output));}
    {Fixture bad(u"relative.flp");CHECK(!bad.decode(output));}
    CHECK(!rysup::fl_ara::save_guard_testing::decodeSaveUnicodePath(a.data()+1,output));
    CHECK(!rysup::fl_ara::save_guard_testing::decodeSaveUnicodePath(1,output));
    std::printf("PASS %u native Save Unicode header controls; CP0/2 and1200/2 only; no DAW/image/file effects\n",checks);
    return 0;
}
