#include "RegistrationReadThrough.h"
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cerrno>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rysup::fl_ara { namespace {
using Key=std::uintptr_t; using Word=char16_t; using Long=std::int32_t; using DWord=std::uint32_t;
constexpr Key hkcu=2; // Actual WinBridge ABI, experimentally verified, not Win32's constant.
constexpr Long failure=1, denied=5, moreData=234;
constexpr const char* winBridgeSHA="433f8dc07e2fab6b1786c7963f7fabe01d86bce75582019e4de0c82dfff5c8f2";
constexpr std::size_t limit=32*1024*1024;
// The helper may initialize this adapter from an earlier translation-unit
// constructor. A namespace-global std::u16string is not initialized then.
const std::u16string& registrationRoot(){static const std::u16string root=u"software\\image-line\\registrations";return root;}
struct Value { std::u16string name,text; unsigned xmlType=0; };
struct Node { std::vector<std::u16string> children; std::vector<Value> values; };
struct State {
    std::recursive_mutex mutex; std::map<std::u16string,Node> nodes;
    std::map<Key,std::u16string> fake,ordinary{{hkcu,u""}};
    std::map<std::string,void*> originals; std::string bridge;
    Key next=0x5259535200000001ULL; RegistrationReadStats stats;
};
State& state(){static State s;return s;}
std::string physical(const std::string& path){char* p=realpath(path.c_str(),nullptr);if(!p)return{};std::string out=p;free(p);return out;}
std::u16string units(NSString* s){if(!s)return{};std::u16string out(s.length,u'\0');if(s.length)[s getCharacters:reinterpret_cast<unichar*>(out.data()) range:NSMakeRange(0,s.length)];return out;}
std::u16string lower(const std::u16string& s){NSString* x=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(s.data()) length:s.size()];return units(x.lowercaseString);}
bool readWide(const Word* p,std::u16string& out){out.clear();if(!p)return true;for(std::size_t i=0;i<32768;++i){if(!p[i])return true;out+=p[i];}return false;}
bool normalize(const std::u16string& input,std::u16string& out){out.clear();std::u16string part;auto commit=[&]{if(part.empty())return true;if(part==u"."||part==u"..")return false;if(!out.empty())out+=u'\\';out+=lower(part);part.clear();return true;};for(auto c:input){if(c==u'\\'||c==u'/'){if(!commit())return false;}else part+=c;}return commit();}
bool auth(const std::u16string& path){const auto& root=registrationRoot();return path==root||(path.size()>root.size()&&path.compare(0,root.size(),root)==0&&path[root.size()]==u'\\');}
bool resolve(Key key,const Word* sub,std::u16string& path){auto& s=state();std::u16string suffix;if(!readWide(sub,suffix))return false;auto a=s.fake.find(key);auto b=s.ordinary.find(key);if(a==s.fake.end()&&b==s.ordinary.end())return false;const auto& parent=a!=s.fake.end()?a->second:b->second;if(!suffix.empty()&&(suffix.front()==u'\\'||suffix.front()==u'/'))return normalize(suffix,path);return normalize(parent+(parent.empty()||suffix.empty()?u"":u"\\")+suffix,path);}
bool fake(Key k){return state().fake.count(k)!=0;}
bool affected(Key k,const Word* sub=nullptr){if(fake(k))return true;std::u16string path;return resolve(k,sub,path)&&auth(path);}
template<class T>T original(const char* name){auto it=state().originals.find(name);return it==state().originals.end()?nullptr:reinterpret_cast<T>(it->second);}
std::string shaFD(int fd,int& readError){readError=0;CC_SHA256_CTX hash;CC_SHA256_Init(&hash);std::array<unsigned char,65536> bytes{};ssize_t n;while((n=read(fd,bytes.data(),bytes.size()))>0)CC_SHA256_Update(&hash,bytes.data(),static_cast<CC_LONG>(n));if(n<0){readError=errno;return{};}unsigned char digest[32];CC_SHA256_Final(digest,&hash);const char* hex="0123456789abcdef";std::string out;for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;}
bool bridgeSealed(const std::string& path,RegistrationReadStats& stats){
    stats.stage=RegistrationInitStage::bridgeOpen;
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){stats.immediateErrno=errno;stats.stage=RegistrationInitStage::bridgeOpenFailed;return false;}
    stats.stage=RegistrationInitStage::bridgeRead;int readError=0;const auto digest=shaFD(fd,readError);close(fd);
    if(readError){stats.immediateErrno=readError;stats.stage=RegistrationInitStage::bridgeReadFailed;return false;}
    if(digest!=winBridgeSHA){stats.stage=RegistrationInitStage::bridgeSealMismatch;return false;}return true;
}
}
}

// Parse only the permitted registration subtree; no serialization API exists.
@interface RysUpRegistrationParser : NSObject<NSXMLParserDelegate> {
@public std::map<std::u16string,rysup::fl_ara::Node>* output;
@public std::vector<std::u16string> keys;
@public bool okay,insideValue; unsigned type;
@public rysup::fl_ara::RegistrationReadStats* diagnostics;
@public std::u16string valueName,valueText,valuePath;
}
@end
@implementation RysUpRegistrationParser
- (instancetype)init { if((self=[super init])){okay=true;insideValue=false;output=nullptr;diagnostics=nullptr;}return self; }
- (void)parser:(NSXMLParser*)p didStartElement:(NSString*)element namespaceURI:(NSString*)uri qualifiedName:(NSString*)qualified attributes:(NSDictionary<NSString*,NSString*>*)attrs {
    using namespace rysup::fl_ara;(void)uri;(void)qualified;
    if(insideValue){if(diagnostics)diagnostics->stage=RegistrationInitStage::parserNestedValue;okay=false;[p abortParsing];return;}
    if([element isEqualToString:@"Key"]){keys.push_back(units(attrs[@"Name"]));std::u16string path;for(std::size_t i=0;i<keys.size();++i)if(lower(keys[i])==u"hkey_current_user"){for(std::size_t j=i+1;j<keys.size();++j){if(!path.empty())path+=u'\\';path+=keys[j];}break;}
        std::u16string normalized;if(normalize(path,normalized)&&auth(normalized)){if(output->count(normalized)){if(diagnostics)diagnostics->stage=RegistrationInitStage::parserDuplicateNode;okay=false;[p abortParsing];return;}output->emplace(normalized,Node{});auto at=normalized.rfind(u'\\');if(at!=std::u16string::npos){const auto parent=normalized.substr(0,at);if(auth(parent)){auto& child=(*output)[parent].children;if(std::find_if(child.begin(),child.end(),[&](const auto& n){return lower(n)==lower(keys.back());})!=child.end()){if(diagnostics)diagnostics->stage=RegistrationInitStage::parserDuplicateNode;okay=false;[p abortParsing];return;}child.push_back(keys.back());}}}
    }else if([element isEqualToString:@"Value"]){std::u16string path;for(std::size_t i=0;i<keys.size();++i)if(lower(keys[i])==u"hkey_current_user"){for(std::size_t j=i+1;j<keys.size();++j){if(!path.empty())path+=u'\\';path+=keys[j];}break;}
        if(normalize(path,valuePath)&&auth(valuePath)){insideValue=true;valueName=units(attrs[@"Name"]);valueText.clear();NSString* t=attrs[@"Type"];if(!t||t.length!=1||[t characterAtIndex:0]<'0'||[t characterAtIndex:0]>'5'){if(diagnostics)diagnostics->stage=RegistrationInitStage::parserTypeRejected;okay=false;[p abortParsing];return;}type=[t characterAtIndex:0]-'0';}
    }
}
- (void)parser:(NSXMLParser*)p foundCharacters:(NSString*)text {if(insideValue){valueText+=rysup::fl_ara::units(text);if(valueText.size()>4*1024*1024){if(diagnostics)diagnostics->stage=rysup::fl_ara::RegistrationInitStage::parserTextLimit;okay=false;[p abortParsing];}}}
- (void)parser:(NSXMLParser*)p didEndElement:(NSString*)element namespaceURI:(NSString*)uri qualifiedName:(NSString*)qualified {
    using namespace rysup::fl_ara;(void)uri;(void)qualified;
    if([element isEqualToString:@"Value"]&&insideValue){auto& values=(*output)[valuePath].values;if(std::find_if(values.begin(),values.end(),[&](const auto& v){return lower(v.name)==lower(valueName);})!=values.end()||values.size()>4096){if(diagnostics)diagnostics->stage=values.size()>4096?RegistrationInitStage::parserValueCountLimit:RegistrationInitStage::parserDuplicateValue;okay=false;[p abortParsing];return;}values.push_back({valueName,valueText,type});insideValue=false;valueText.clear();}
    if([element isEqualToString:@"Key"]&&!keys.empty())keys.pop_back();
}
- (void)parser:(NSXMLParser*)p parseErrorOccurred:(NSError*)error {
    if(diagnostics){diagnostics->xmlErrorCode=error.code;diagnostics->xmlErrorLine=p.lineNumber>0?static_cast<std::uint64_t>(p.lineNumber):0;diagnostics->xmlErrorColumn=p.columnNumber>0?static_cast<std::uint64_t>(p.columnNumber):0;if(diagnostics->stage==rysup::fl_ara::RegistrationInitStage::parsing)diagnostics->stage=rysup::fl_ara::RegistrationInitStage::parserXMLError;}okay=false;
}
@end

namespace rysup::fl_ara { namespace {
bool payload(const Value& v,DWord& type,DWord& reported,std::vector<unsigned char>& bytes){
    bytes.clear();static const DWord wire[]={0,4,1,3,7,11};if(v.xmlType>5)return false;type=wire[v.xmlType];
    if(type==1){if(v.text.size()>std::numeric_limits<DWord>::max()/4)return false;reported=static_cast<DWord>(v.text.size()*4);for(auto c:v.text){bytes.push_back(c&255);bytes.push_back(c>>8);}bytes.push_back(0);bytes.push_back(0);return true;}
    if(type==3){reported=static_cast<DWord>(v.text.size()/2);auto hex=[](char16_t c)->int{if(c>=u'0'&&c<=u'9')return c-u'0';if(c>=u'a'&&c<=u'f')return c-u'a'+10;if(c>=u'A'&&c<=u'F')return c-u'A'+10;return -1;};for(std::size_t i=0;i+1<v.text.size();i+=2){int a=hex(v.text[i]),b=hex(v.text[i+1]);if(a<0||b<0)return false;bytes.push_back(static_cast<unsigned char>(a*16+b));}return true;}
    if(type==4){std::string text;for(auto c:v.text){if(c>127)return false;text+=static_cast<char>(c);}try{std::size_t used=0;int base=10;if(text.size()>1&&text[0]=='$'){text.erase(0,1);base=16;}auto number=std::stoll(text,&used,base);if(used!=text.size()||number<std::numeric_limits<std::int32_t>::min()||number>std::numeric_limits<DWord>::max())return false;DWord value=static_cast<DWord>(number);reported=4;for(unsigned i=0;i<4;++i)bytes.push_back((value>>(8*i))&255);return true;}catch(...){return false;}}
    return false; // The actual exported vendor Query does not handle MULTI/QWORD.
}
Long openKey(Key key,const Word* sub,DWord options,DWord access,Key* out){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(!out)return failure;*out=0;std::u16string path;const bool resolved=resolve(key,sub,path);if(fake(key)&&(!resolved||!auth(path)))return failure;if(resolved&&auth(path)){auto node=s.nodes.find(path);if(node==s.nodes.end())return failure;for(const auto& item:s.fake)if(item.second==path){*out=item.first;return 0;}Key id=s.next++;s.fake[id]=path;*out=id;return 0;}using Fn=Long(*)(Key,const Word*,DWord,DWord,Key*);auto fn=original<Fn>("RegOpenKeyExW");if(!fn)return failure;auto result=fn(key,sub,options,access,out);if(result==0&&*out&&resolve(key,sub,path))s.ordinary[*out]=path;return result;}
Long closeKey(Key key){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(fake(key))return 0;using Fn=Long(*)(Key);auto fn=original<Fn>("RegCloseKey");return fn?fn(key):failure;}
Long flushKey(Key key){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(fake(key))return 0;using Fn=Long(*)(Key);auto fn=original<Fn>("RegFlushKey");return fn?fn(key):failure;}
Long query(Key key,const Word* name,DWord* reserved,DWord* type,void* data,DWord* size){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);auto k=s.fake.find(key);if(k==s.fake.end()){using Fn=Long(*)(Key,const Word*,DWord*,DWord*,void*,DWord*);auto fn=original<Fn>("RegQueryValueExW");return fn?fn(key,name,reserved,type,data,size):failure;}if(!type||!size)return failure;std::u16string n;if(!readWide(name,n))return failure;auto& values=s.nodes.at(k->second).values;auto v=std::find_if(values.begin(),values.end(),[&](const auto& value){return lower(value.name)==lower(n);});if(v==values.end())return failure;DWord wire=0,reported=0;std::vector<unsigned char> bytes;if(!payload(*v,wire,reported,bytes)){*type=wire;return failure;}const auto capacity=*size;*type=wire;*size=reported;++s.stats.registrationReads;if(data){if(capacity<bytes.size())return moreData;std::memcpy(data,bytes.data(),bytes.size());}return 0;}
Long enumerate(Key key,DWord index,Word* name,DWord* length,bool values){auto& s=state();auto k=s.fake.find(key);if(k==s.fake.end())return -1;if(!name||!length)return failure;const auto& node=s.nodes.at(k->second);const auto count=values?node.values.size():node.children.size();if(index>=count)return failure;const auto& text=values?node.values[index].name:node.children[index];if(*length<=text.size())return moreData;std::copy(text.begin(),text.end(),name);name[text.size()]=0;return 0;}
Long enumKey(Key k,DWord i,Word* n,DWord* len,DWord* r,Word* cls,DWord* cl,DWord* time){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);auto result=enumerate(k,i,n,len,false);if(result!=-1)return result;using Fn=Long(*)(Key,DWord,Word*,DWord*,DWord*,Word*,DWord*,DWord*);auto fn=original<Fn>("RegEnumKeyExW");return fn?fn(k,i,n,len,r,cls,cl,time):failure;}
Long enumValue(Key k,DWord i,Word* n,DWord* len,DWord* r,DWord* t,void* data,DWord* size){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);auto result=enumerate(k,i,n,len,true);if(result!=-1)return result;using Fn=Long(*)(Key,DWord,Word*,DWord*,DWord*,DWord*,void*,DWord*);auto fn=original<Fn>("RegEnumValueW");return fn?fn(k,i,n,len,r,t,data,size):failure;}
Long info(Key k,Word* cls,DWord* cl,DWord* r,DWord* sub,DWord* maxSub,DWord* maxClass,DWord* vals,DWord* maxName,DWord* maxValue,DWord* security,void* time){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);auto it=s.fake.find(k);if(it==s.fake.end()){using Fn=Long(*)(Key,Word*,DWord*,DWord*,DWord*,DWord*,DWord*,DWord*,DWord*,DWord*,DWord*,void*);auto fn=original<Fn>("RegQueryInfoKeyW");return fn?fn(k,cls,cl,r,sub,maxSub,maxClass,vals,maxName,maxValue,security,time):failure;}const auto& node=s.nodes.at(it->second);if(sub)*sub=static_cast<DWord>(node.children.size());if(vals)*vals=static_cast<DWord>(node.values.size());if(maxSub){*maxSub=0;for(const auto& n:node.children)*maxSub=std::max(*maxSub,static_cast<DWord>(n.size()));}if(maxName){*maxName=0;for(const auto& v:node.values)*maxName=std::max(*maxName,static_cast<DWord>(v.name.size()));}if(maxValue){*maxValue=0;for(const auto& v:node.values){DWord type=0,count=0;std::vector<unsigned char> bytes;if(payload(v,type,count,bytes))*maxValue=std::max(*maxValue,static_cast<DWord>(bytes.size()));}}return 0;}
Long create(Key k,const Word* path,DWord reserved,Word* cls,DWord options,DWord access,void* security,Key* out,DWord* disposition){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k,path)){if(out)*out=0;++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*,DWord,Word*,DWord,DWord,void*,Key*,DWord*);auto fn=original<Fn>("RegCreateKeyExW");if(!fn)return failure;auto result=fn(k,path,reserved,cls,options,access,security,out,disposition);std::u16string normalized;if(result==0&&out&&*out&&resolve(k,path,normalized))s.ordinary[*out]=normalized;return result;}
Long set(Key k,const Word* n,DWord r,DWord t,const void* d,DWord count){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*,DWord,DWord,const void*,DWord);auto fn=original<Fn>("RegSetValueExW");return fn?fn(k,n,r,t,d,count):failure;}
Long deleteKey(Key k,const Word* path){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k,path)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*);auto fn=original<Fn>("RegDeleteKeyW");return fn?fn(k,path):failure;}
Long deleteKeyEx(Key k,const Word* path,DWord access,DWord r){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k,path)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*,DWord,DWord);auto fn=original<Fn>("RegDeleteKeyExW");return fn?fn(k,path,access,r):failure;}
Long deleteValue(Key k,const Word* name){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*);auto fn=original<Fn>("RegDeleteValueW");return fn?fn(k,name):failure;}
Long load(Key k,const Word* path,const Word* file){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k,path)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*,const Word*);auto fn=original<Fn>("RegLoadKeyW");return fn?fn(k,path,file):failure;}
Long unload(Key k,const Word* path){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k,path)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*);auto fn=original<Fn>("RegUnLoadKeyW");return fn?fn(k,path):failure;}
Long replace(Key k,const Word* path,const Word* newer,const Word* old){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k,path)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*,const Word*,const Word*);auto fn=original<Fn>("RegReplaceKeyW");return fn?fn(k,path,newer,old):failure;}
Long restore(Key k,const Word* file,DWord flags){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*,DWord);auto fn=original<Fn>("RegRestoreKeyW");return fn?fn(k,file,flags):failure;}
Long save(Key k,const Word* file,void* security){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k)){++s.stats.deniedRegistrationWrites;return denied;}using Fn=Long(*)(Key,const Word*,void*);auto fn=original<Fn>("RegSaveKeyW");return fn?fn(k,file,security):failure;}
void writeString(Key k,const Word* path,const Word* name,const Word* text){auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(affected(k,path)){++s.stats.deniedRegistrationWrites;return;}using Fn=void(*)(Key,const Word*,const Word*,const Word*);auto fn=original<Fn>("RegWriteStringValue");if(fn)fn(k,path,name,text);}
} // namespace
bool initializeRegistrationReadThrough(const RegistrationReadOptions& options) noexcept {try{
    auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);
    if(s.stats.initialized){s.stats.stage=RegistrationInitStage::alreadyInitialized;return false;}
    auto& diagnostic=s.stats;diagnostic.immediateErrno=0;diagnostic.bytesRead=0;diagnostic.xmlErrorCode=0;diagnostic.xmlErrorLine=0;diagnostic.xmlErrorColumn=0;diagnostic.parsedRegistrationNodeCount=0;diagnostic.parsedRegistrationValueCount=0;
    const auto reject=[&](RegistrationInitStage stage,int code=0){diagnostic.stage=stage;diagnostic.immediateErrno=code;return false;};
    diagnostic.stage=RegistrationInitStage::sourceMetadata;struct stat supplied{};
    if(lstat(options.originalRegistryPath.c_str(),&supplied))return reject(RegistrationInitStage::sourceLstatFailed,errno);
    if(!S_ISREG(supplied.st_mode))return reject(RegistrationInitStage::sourceNotRegular);
    diagnostic.stage=RegistrationInitStage::sourceRealpath;const auto source=physical(options.originalRegistryPath);
    if(source.empty())return reject(RegistrationInitStage::sourceRealpathFailed,errno);
    diagnostic.stage=RegistrationInitStage::bridgeRealpath;const auto bridge=physical(options.exactWinBridgePath);
    if(bridge.empty())return reject(RegistrationInitStage::bridgeRealpathFailed,errno);
    if(!bridgeSealed(bridge,diagnostic))return false;
#if !defined(RYSUP_REGISTRY_ADAPTER_TESTING) || !RYSUP_REGISTRY_ADAPTER_TESTING
    diagnostic.stage=RegistrationInitStage::ownerLookup;errno=0;const passwd* user=getpwuid(getuid());
    if(!user)return reject(RegistrationInitStage::ownerLookupFailed,errno);
    if(!user->pw_dir)return reject(RegistrationInitStage::ownerHomeMissing);
    diagnostic.stage=RegistrationInitStage::ownerExpectedRealpath;const auto expected=physical(std::string(user->pw_dir)+"/Library/Preferences/Image-Line/reg.xml");
    if(expected.empty())return reject(RegistrationInitStage::ownerExpectedRealpathFailed,errno);
    if(source!=expected)return reject(RegistrationInitStage::ownerPathMismatch);
#endif
    diagnostic.stage=RegistrationInitStage::sourceOpen;int fd=open(source.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return reject(RegistrationInitStage::sourceOpenFailed,errno);
    const auto rejectFile=[&](RegistrationInitStage stage,int code=0){close(fd);return reject(stage,code);};
    diagnostic.stage=RegistrationInitStage::sourceMetadataBefore;struct stat before{},after{};
    if(fstat(fd,&before))return rejectFile(RegistrationInitStage::sourceFstatBeforeFailed,errno);
    if(!S_ISREG(before.st_mode))return rejectFile(RegistrationInitStage::sourceNotRegular);
    if(before.st_uid!=getuid())return rejectFile(RegistrationInitStage::sourceOwnerMismatch);
    if(before.st_mode&0022)return rejectFile(RegistrationInitStage::sourceModeRejected);
    if(before.st_size<0||static_cast<std::uint64_t>(before.st_size)>limit)return rejectFile(RegistrationInitStage::sourceSizeRejected);
    NSMutableData* data=[NSMutableData data];std::array<unsigned char,65536> buffer{};ssize_t n=0;diagnostic.stage=RegistrationInitStage::sourceRead;
    while((n=read(fd,buffer.data(),buffer.size()))>0){diagnostic.bytesRead+=static_cast<std::uint64_t>(n);[data appendBytes:buffer.data() length:static_cast<NSUInteger>(n)];if(data.length>limit)return rejectFile(RegistrationInitStage::sourceReadLimit);}
    if(n<0)return rejectFile(RegistrationInitStage::sourceReadFailed,errno);
    diagnostic.stage=RegistrationInitStage::sourceMetadataAfter;if(fstat(fd,&after))return rejectFile(RegistrationInitStage::sourceFstatAfterFailed,errno);
    const bool unchanged=before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec&&before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec;
    close(fd);if(!unchanged)return reject(RegistrationInitStage::sourceChanged);
    diagnostic.stage=RegistrationInitStage::sourceEncoding;NSString* text=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding];
    if(!text)return reject(RegistrationInitStage::sourceUTF8Rejected);
    if([text.uppercaseString containsString:@"<!DOCTYPE"])return reject(RegistrationInitStage::sourceDoctypeRejected);
    if([text rangeOfString:@"\0"].location!=NSNotFound)return reject(RegistrationInitStage::sourceNulRejected);
    std::map<std::u16string,Node> parsed;RysUpRegistrationParser* delegate=[RysUpRegistrationParser new];delegate->output=&parsed;delegate->diagnostics=&diagnostic;
    NSXMLParser* parser=[[NSXMLParser alloc]initWithData:data];parser.shouldResolveExternalEntities=NO;parser.delegate=delegate;diagnostic.stage=RegistrationInitStage::parsing;
    const bool parsedOkay=[parser parse]&&delegate->okay;
    diagnostic.parsedRegistrationNodeCount=parsed.size();for(const auto& item:parsed)diagnostic.parsedRegistrationValueCount+=item.second.values.size();
    [data resetBytesInRange:NSMakeRange(0,data.length)];if(!parsedOkay)return false;
    s.nodes=std::move(parsed);s.bridge=bridge;s.stats.initialized=true;s.stats.registrationBranchPresent=s.nodes.count(registrationRoot())!=0;for(const auto& item:s.nodes)s.stats.registrationValueCount+=item.second.values.size();diagnostic.stage=RegistrationInitStage::ready;return true;
}catch(...){return false;}}

void* observeRegistrationSymbol(const char* name,void* resolved) noexcept {try{auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);if(!s.stats.initialized||!name||!resolved)return resolved;Dl_info image{};if(!dladdr(resolved,&image)||physical(image.dli_fname)!=s.bridge)return resolved;
#define RYSUP_REGISTRY_SYMBOL(label,function) if(std::strcmp(name,label)==0){s.originals[label]=resolved;return reinterpret_cast<void*>(&function);}
    RYSUP_REGISTRY_SYMBOL("RegOpenKeyExW",openKey) RYSUP_REGISTRY_SYMBOL("RegCloseKey",closeKey) RYSUP_REGISTRY_SYMBOL("RegFlushKey",flushKey)
    RYSUP_REGISTRY_SYMBOL("RegQueryValueExW",query) RYSUP_REGISTRY_SYMBOL("RegEnumKeyExW",enumKey) RYSUP_REGISTRY_SYMBOL("RegEnumValueW",enumValue) RYSUP_REGISTRY_SYMBOL("RegQueryInfoKeyW",info)
    RYSUP_REGISTRY_SYMBOL("RegCreateKeyExW",create) RYSUP_REGISTRY_SYMBOL("RegSetValueExW",set) RYSUP_REGISTRY_SYMBOL("RegDeleteKeyW",deleteKey) RYSUP_REGISTRY_SYMBOL("RegDeleteKeyExW",deleteKeyEx) RYSUP_REGISTRY_SYMBOL("RegDeleteValueW",deleteValue)
    RYSUP_REGISTRY_SYMBOL("RegLoadKeyW",load) RYSUP_REGISTRY_SYMBOL("RegUnLoadKeyW",unload) RYSUP_REGISTRY_SYMBOL("RegReplaceKeyW",replace) RYSUP_REGISTRY_SYMBOL("RegRestoreKeyW",restore) RYSUP_REGISTRY_SYMBOL("RegSaveKeyW",save) RYSUP_REGISTRY_SYMBOL("RegWriteStringValue",writeString)
#undef RYSUP_REGISTRY_SYMBOL
    return resolved;
}catch(...){return resolved;}}
RegistrationReadStats registrationReadStats() noexcept {auto& s=state();std::lock_guard<std::recursive_mutex> guard(s.mutex);return s.stats;}
} // namespace rysup::fl_ara
