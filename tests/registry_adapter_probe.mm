#include "RegistrationReadThrough.h"
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pwd.h>
#include <cerrno>
#include <unistd.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
using K=std::uintptr_t;using W=char16_t;using D=std::uint32_t;using L=std::int32_t;
static unsigned checks=0;
static void need(bool okay,const char* message){++checks;if(!okay)throw std::runtime_error(message);}
template<class T>T symbol(void* module,const char* name){auto p=dlsym(module,name);need(p!=nullptr,"missing real vendor symbol");return reinterpret_cast<T>(rysup::fl_ara::observeRegistrationSymbol(name,p));}
int main(int argc,char** argv){@autoreleasepool{try{
 need(argc==3,"requires exact vendor bridge and synthetic source fixture");
 const auto user=getpwuid(getuid());need(user&&user->pw_dir,"real user home lookup failed");
 const auto realRegistry=std::string(user->pw_dir)+"/Library/Preferences/Image-Line/reg.xml";
 errno=0;int deniedFD=open(realRegistry.c_str(),O_RDONLY);int deniedErrno=errno;if(deniedFD>=0)close(deniedFD);
 need(deniedFD<0&&(deniedErrno==EPERM||deniedErrno==EACCES),"child sandbox did not deny original-registry open");
 const auto outside=std::string("/private/tmp/registry-adapter-outside-")+std::to_string(getpid());
 errno=0;int writeFD=open(outside.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);int writeErrno=errno;if(writeFD>=0){close(writeFD);unlink(outside.c_str());}
 need(writeFD<0&&(writeErrno==EPERM||writeErrno==EACCES),"child sandbox did not deny outside-run write");
 need(rysup::fl_ara::initializeRegistrationReadThrough({argv[2],argv[1]}),"adapter initialization failed");
 void* module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);need(module!=nullptr,"vendor bridge load failed");
 auto open=symbol<L(*)(K,const W*,D,D,K*)>(module,"RegOpenKeyExW");auto query=symbol<L(*)(K,const W*,D*,D*,void*,D*)>(module,"RegQueryValueExW");
 auto close=symbol<L(*)(K)>(module,"RegCloseKey");auto flush=symbol<L(*)(K)>(module,"RegFlushKey");
 auto enumKey=symbol<L(*)(K,D,W*,D*,D*,W*,D*,void*)>(module,"RegEnumKeyExW");auto enumValue=symbol<L(*)(K,D,W*,D*,D*,D*,void*,D*)>(module,"RegEnumValueW");
 auto info=symbol<L(*)(K,W*,D*,D*,D*,D*,D*,D*,D*,D*,D*,void*)>(module,"RegQueryInfoKeyW");
 auto create=symbol<L(*)(K,const W*,D,W*,D,D,void*,K*,D*)>(module,"RegCreateKeyExW");auto set=symbol<L(*)(K,const W*,D,D,const void*,D)>(module,"RegSetValueExW");
 auto del=symbol<L(*)(K,const W*)>(module,"RegDeleteKeyW");auto delEx=symbol<L(*)(K,const W*,D,D)>(module,"RegDeleteKeyExW");auto delValue=symbol<L(*)(K,const W*)>(module,"RegDeleteValueW");
 auto load=symbol<L(*)(K,const W*,const W*)>(module,"RegLoadKeyW");auto unload=symbol<L(*)(K,const W*)>(module,"RegUnLoadKeyW");auto replace=symbol<L(*)(K,const W*,const W*,const W*)>(module,"RegReplaceKeyW");auto restore=symbol<L(*)(K,const W*,D)>(module,"RegRestoreKeyW");auto save=symbol<L(*)(K,const W*,void*)>(module,"RegSaveKeyW");
 auto write=symbol<void(*)(K,const W*,const W*,const W*)>(module,"RegWriteStringValue");auto saveAll=reinterpret_cast<void(*)()>(dlsym(module,"RegSaveAllChanges"));need(saveAll!=nullptr,"missing saveAll");
 auto storage=reinterpret_cast<const W*(*)()>(dlsym(module,"RegGetStoragePath"));need(storage!=nullptr,"missing storage");const auto storageText=std::u16string(storage());need(storageText.find(u"registry-adapter-")!=std::u16string::npos,"vendor storage did not remain in own synthetic profile");
 K parent=0,auth=0,node=0,privateKey=0;
 need(open(2,u"Software\\Image-Line",0,0x20019,&parent)==0,"private parent open failed");
 need(open(parent,u"rEgIsTrAtIoNs",0,0x20019,&auth)==0,"case-insensitive registration relative open failed");
 need(open(auth,u"SyntheticOnly",0,0x20019,&node)==0,"synthetic registration child open failed");
 need(open(node,u"\\Software\\Image-Line\\Other",0,0x20019,&privateKey)==1,"synthetic handle escaped protected subtree");
 std::array<unsigned char,512> buffer;D type=0,size=0;
 auto read=[&](const W* name,D expectedType,D expectedSize,const std::vector<unsigned char>& expected){buffer.fill(0xa5);type=0;size=buffer.size();need(query(node,name,nullptr,&type,buffer.data(),&size)==0,"synthetic query failed");need(type==expectedType&&size==expectedSize,"vendor-compatible query metadata differs");need(std::memcmp(buffer.data(),expected.data(),expected.size())==0,"query payload differs");need(buffer[expected.size()]==0xa5,"query overwrote expected payload capacity");};
 read(u"Ascii",1,24,{0x61,0,0x62,0,0x63,0,0x58,0,0x59,0,0x5a,0,0,0});
 read(u"Name\U0001f600",1,16,{0x41,0,0x3d,0xd8,0,0xde,0x5a,0,0,0});
 read(u"Binary",3,6,{0,1,0x7f,0x80,0xfe,0xff});read(u"Number",4,4,{0x78,0x56,0x34,0x12});read(u"OddHex",3,1,{0x0a});read(u"Empty",1,0,{0,0});
 size=0;type=0;need(query(node,u"Ascii",nullptr,&type,nullptr,&size)==0&&size==24&&type==1,"null data size query differs");
 buffer.fill(0xa5);size=2;need(query(node,u"Ascii",nullptr,&type,buffer.data(),&size)==234&&buffer[0]==0xa5,"undersized output did not fail without writes");
 size=buffer.size();need(query(node,u"MalformedHex",nullptr,&type,buffer.data(),&size)==1,"malformed hex did not fail closed");
 size=buffer.size();need(query(node,u"Unsupported",nullptr,&type,buffer.data(),&size)==1&&type==11,"unsupported vendor type did not fail closed");
 size=buffer.size();need(query(node,u"textualvalue:Literal",nullptr,&type,buffer.data(),&size)==0,"literal value prefix was rewritten");size=buffer.size();need(query(node,u"Literal",nullptr,&type,buffer.data(),&size)==1,"missing unprefixed value was fabricated");
 need(close(node)==0&&close(node)==0,"close no-op differs");size=buffer.size();need(query(node,u"Ascii",nullptr,&type,buffer.data(),&size)==0,"post-close vendor handle validity differs");need(flush(node)==0,"fake flush failed");
 std::array<W,128> names;names.fill(u'X');D length=names.size();need(enumKey(auth,0,names.data(),&length,nullptr,nullptr,nullptr,nullptr)==0&&std::u16string(names.data())==u"SyntheticOnly"&&length==names.size(),"key enumeration differs");
 names.fill(u'X');length=names.size();D outType=0xabad,outSize=0xbeef;need(enumValue(node,0,names.data(),&length,nullptr,&outType,buffer.data(),&outSize)==0&&std::u16string(names.data())==u"Ascii"&&length==names.size()&&outType==0xabad&&outSize==0xbeef,"names-only value enumeration differs");
 length=2;names.fill(u'X');need(enumValue(node,0,names.data(),&length,nullptr,nullptr,nullptr,nullptr)==234&&names[0]==u'X',"short enum output was not protected");
 D sub=0,maxSub=0,values=0,maxName=0,maxData=0,classLen=7,maxClass=99,security=88;std::uint64_t time=0xdeadbeef;
 need(info(node,nullptr,&classLen,nullptr,&sub,&maxSub,&maxClass,&values,&maxName,&maxData,&security,&time)==0&&values==10&&sub==0&&classLen==7&&maxClass==99&&security==88&&time==0xdeadbeef,"query info changed unsupported outputs/counts");
 K created=123;D disposition=0;need(create(node,u"NoWrite",0,nullptr,0,0,nullptr,&created,&disposition)==5&&created==0,"create allowed registration write");
 need(set(node,u"NoWrite",0,1,"x",1)==5,"set allowed registration write");need(del(node,u"x")==5&&delEx(node,u"x",0,0)==5&&delValue(node,u"Ascii")==5,"delete allowed registration write");
 need(load(node,u"x",u"x")==5&&unload(node,u"x")==5&&replace(node,u"x",u"x",u"x")==5&&restore(node,u"x",0)==5&&save(node,u"x",nullptr)==5,"load/save/restore allowed registration write");
 write(2,u"Software\\Image-Line\\Registrations\\New",u"NoWrite",u"MUST_NOT_PERSIST");
 need(open(2,u"Software\\Image-Line\\Shared\\Paths",0,0x20019,&privateKey)==0,"ordinary private key was blocked");size=buffer.size();need(query(privateKey,u"Private",nullptr,&type,buffer.data(),&size)==0&&std::u16string(reinterpret_cast<const W*>(buffer.data()))==u"PRIVATE_ONLY","ordinary private query did not forward unchanged");
 write(2,u"Software\\Image-Line\\Shared\\Paths",u"NewPrivate",u"PRIVATE_WRITE_ALLOWED");saveAll();
 auto stats=rysup::fl_ara::registrationReadStats();need(stats.initialized&&stats.registrationBranchPresent&&stats.registrationValueCount==10&&stats.deniedRegistrationWrites>=11,"registration diagnostics/counts differ");
 std::printf("{\"checks\":%u,\"passed\":true,\"registration_reads\":%llu,\"denied_writes\":%llu,\"fake_values\":%llu}\n",checks,(unsigned long long)stats.registrationReads,(unsigned long long)stats.deniedRegistrationWrites,(unsigned long long)stats.registrationValueCount);return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"synthetic adapter check failed: %s (check%u)\n",e.what(),checks);return 1;}}}
