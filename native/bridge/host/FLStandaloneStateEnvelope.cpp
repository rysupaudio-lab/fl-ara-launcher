#include "FLStandaloneStateEnvelope.h"
#include <charconv>
#include <sstream>
#include <map>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace rysup::ara_lab {
namespace {
constexpr char magic[]="RYSFLAPP";
constexpr std::size_t headerBytes=52;
constexpr std::size_t maxText=65536, maxRecords=100000;
[[noreturn]] void fail(const char* what){throw std::runtime_error(std::string("native ARA envelope: ")+what);}
void require(bool condition,const char* what){if(!condition)fail(what);}
template<class T> class CF final {
public:
    explicit CF(T v=nullptr):v_(v){} ~CF(){if(v_)CFRelease(v_);}
    CF(const CF&)=delete;CF& operator=(const CF&)=delete;
    CF(CF&& o) noexcept:v_(o.v_){o.v_=nullptr;}
    T get()const{return v_;} operator T()const{return v_;}
private:T v_;
};
using Dict=CF<CFMutableDictionaryRef>;using Array=CF<CFMutableArrayRef>;
Dict dict(){return Dict(CFDictionaryCreateMutable(nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));}
Array array(){return Array(CFArrayCreateMutable(nullptr,0,&kCFTypeArrayCallBacks));}
void textValid(const std::string& v,bool nonempty=false){
    require(v.size()<=maxText && (!nonempty||!v.empty()) && v.find('\0')==std::string::npos,"invalid text length/NUL");
    CF<CFStringRef> s(CFStringCreateWithBytes(nullptr,reinterpret_cast<const UInt8*>(v.data()),v.size(),kCFStringEncodingUTF8,false));
    require(s.get()!=nullptr,"invalid UTF-8");
}
void setText(CFMutableDictionaryRef d,CFStringRef key,const std::string& v){
    CF<CFStringRef> s(CFStringCreateWithBytes(nullptr,reinterpret_cast<const UInt8*>(v.data()),v.size(),kCFStringEncodingUTF8,false));
    require(s.get()!=nullptr,"invalid UTF-8");CFDictionarySetValue(d,key,s);
}
void setInt(CFMutableDictionaryRef d,CFStringRef k,std::int64_t n){CF<CFNumberRef> v(CFNumberCreate(nullptr,kCFNumberSInt64Type,&n));CFDictionarySetValue(d,k,v);}
void setReal(CFMutableDictionaryRef d,CFStringRef k,double n){CF<CFNumberRef> v(CFNumberCreate(nullptr,kCFNumberDoubleType,&n));CFDictionarySetValue(d,k,v);}
void setData(CFMutableDictionaryRef d,CFStringRef k,const Bytes& b){CF<CFDataRef> v(CFDataCreate(nullptr,b.data(),b.size()));CFDictionarySetValue(d,k,v);}
CFTypeRef get(CFDictionaryRef d,CFStringRef k,CFTypeID type){auto v=CFDictionaryGetValue(d,k);require(v && CFGetTypeID(v)==type,"missing/wrong-type field");return v;}
CFDictionaryRef getDict(CFTypeRef o,CFIndex count){require(o && CFGetTypeID(o)==CFDictionaryGetTypeID(),"expected dictionary");auto d=static_cast<CFDictionaryRef>(o);require(CFDictionaryGetCount(d)==count,"unexpected dictionary fields");return d;}
std::string getText(CFDictionaryRef d,CFStringRef k){
    auto s=static_cast<CFStringRef>(get(d,k,CFStringGetTypeID()));
    require(CFStringGetLength(s)<=static_cast<CFIndex>(maxText),"oversized string");
    CFIndex used=0;const auto n=CFStringGetLength(s);
    require(CFStringGetBytes(s,CFRangeMake(0,n),kCFStringEncodingUTF8,0,false,nullptr,0,&used)==n && used<=static_cast<CFIndex>(maxText),"invalid UTF-8 string");
    std::string out(static_cast<std::size_t>(used),'\0');
    require(CFStringGetBytes(s,CFRangeMake(0,n),kCFStringEncodingUTF8,0,false,reinterpret_cast<UInt8*>(out.data()),used,nullptr)==n,"string decode failed");
    textValid(out);return out;
}
std::int64_t getInt(CFDictionaryRef d,CFStringRef k){
    auto v=static_cast<CFNumberRef>(get(d,k,CFNumberGetTypeID()));require(!CFNumberIsFloatType(v),"integer stored as floating point");
    std::int64_t n=0;require(CFNumberGetValue(v,kCFNumberSInt64Type,&n),"integer overflow");return n;
}
double getReal(CFDictionaryRef d,CFStringRef k){auto v=static_cast<CFNumberRef>(get(d,k,CFNumberGetTypeID()));double n=0;require(CFNumberGetValue(v,kCFNumberDoubleType,&n)&&std::isfinite(n),"invalid real number");return n;}
Bytes getData(CFDictionaryRef d,CFStringRef k){auto v=static_cast<CFDataRef>(get(d,k,CFDataGetTypeID()));auto n=CFDataGetLength(v);require(n>=0 && static_cast<std::size_t>(n)<=kFLStandaloneEnvelopeMaximumBytes,"oversized data");if(!n)return {};auto p=CFDataGetBytePtr(v);return Bytes(p,p+n);}
CFArrayRef getArray(CFDictionaryRef d,CFStringRef k){auto a=static_cast<CFArrayRef>(get(d,k,CFArrayGetTypeID()));require(CFArrayGetCount(a)<=static_cast<CFIndex>(maxRecords),"too many array records");return a;}
bool hashValid(const std::string& s){return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;}
std::array<unsigned char,32> sha(const unsigned char* p,std::size_t n){std::array<unsigned char,32> out{};require(n<=std::numeric_limits<CC_LONG>::max(),"hash input overflow");CC_SHA256(p,static_cast<CC_LONG>(n),out.data());return out;}
void putLE(Bytes& b,std::uint64_t n,unsigned count){for(unsigned i=0;i<count;++i)b.push_back(static_cast<unsigned char>(n>>(i*8)));}
std::uint64_t readLE(const Bytes& b,std::size_t at,unsigned count){require(at<=b.size()&&count<=b.size()-at,"truncated integer");std::uint64_t n=0;for(unsigned i=0;i<count;++i)n|=std::uint64_t(b[at+i])<<(8*i);return n;}
Dict graphValue(const Graph& g){
    auto d=dict();setText(d,CFSTR("documentName"),g.documentName);auto sources=array(),tracks=array(),mods=array(),regions=array();
    for(const auto& s:g.sources){auto v=dict();setText(v,CFSTR("id"),s.id);setText(v,CFSTR("name"),s.name);setInt(v,CFSTR("sampleCount"),s.sampleCount);setReal(v,CFSTR("sampleRate"),s.sampleRate);setInt(v,CFSTR("channelCount"),s.channelCount);CFArrayAppendValue(sources,v);}
    for(const auto& t:g.tracks){auto v=dict();setText(v,CFSTR("id"),t.id);setText(v,CFSTR("name"),t.name);CFArrayAppendValue(tracks,v);}
    for(const auto& m:g.modifications){auto v=dict();setText(v,CFSTR("id"),m.id);setText(v,CFSTR("sourceID"),m.sourceID);setText(v,CFSTR("name"),m.name);CFArrayAppendValue(mods,v);}
    for(const auto& r:g.regions){auto v=dict();setText(v,CFSTR("id"),r.id);setText(v,CFSTR("modificationID"),r.modificationID);setText(v,CFSTR("trackID"),r.trackID);setText(v,CFSTR("name"),r.name);setReal(v,CFSTR("sourceStart"),r.sourceStart);setReal(v,CFSTR("sourceDuration"),r.sourceDuration);setReal(v,CFSTR("playbackStart"),r.playbackStart);setReal(v,CFSTR("playbackDuration"),r.playbackDuration);setInt(v,CFSTR("transformations"),r.transformations);CFArrayAppendValue(regions,v);}
    CFDictionarySetValue(d,CFSTR("sources"),sources);CFDictionarySetValue(d,CFSTR("tracks"),tracks);CFDictionarySetValue(d,CFSTR("modifications"),mods);CFDictionarySetValue(d,CFSTR("regions"),regions);return d;
}
Graph parseGraph(CFDictionaryRef d){
    getDict(d,5);Graph g;g.documentName=getText(d,CFSTR("documentName"));
    auto sources=getArray(d,CFSTR("sources")),tracks=getArray(d,CFSTR("tracks")),mods=getArray(d,CFSTR("modifications")),regions=getArray(d,CFSTR("regions"));
    require(CFArrayGetCount(sources)+CFArrayGetCount(tracks)+CFArrayGetCount(mods)+CFArrayGetCount(regions)<=static_cast<CFIndex>(maxRecords),"too many graph records");
    for(CFIndex i=0;i<CFArrayGetCount(sources);++i){auto v=getDict(CFArrayGetValueAtIndex(sources,i),5);auto channels=getInt(v,CFSTR("channelCount"));require(channels>=1&&channels<=64,"invalid channel count");g.sources.push_back({getText(v,CFSTR("id")),getText(v,CFSTR("name")),getInt(v,CFSTR("sampleCount")),getReal(v,CFSTR("sampleRate")),static_cast<int>(channels)});}
    for(CFIndex i=0;i<CFArrayGetCount(tracks);++i){auto v=getDict(CFArrayGetValueAtIndex(tracks,i),2);g.tracks.push_back({getText(v,CFSTR("id")),getText(v,CFSTR("name"))});}
    for(CFIndex i=0;i<CFArrayGetCount(mods);++i){auto v=getDict(CFArrayGetValueAtIndex(mods,i),3);g.modifications.push_back({getText(v,CFSTR("id")),getText(v,CFSTR("sourceID")),getText(v,CFSTR("name"))});}
    for(CFIndex i=0;i<CFArrayGetCount(regions);++i){auto v=getDict(CFArrayGetValueAtIndex(regions,i),9);auto flags=getInt(v,CFSTR("transformations"));require(flags>=0&&flags<=15,"unknown ARA transformation flags");g.regions.push_back({getText(v,CFSTR("id")),getText(v,CFSTR("modificationID")),getText(v,CFSTR("trackID")),getText(v,CFSTR("name")),getReal(v,CFSTR("sourceStart")),getReal(v,CFSTR("sourceDuration")),getReal(v,CFSTR("playbackStart")),getReal(v,CFSTR("playbackDuration")),static_cast<ARA::ARAPlaybackTransformationFlags>(flags)});}
    return g;
}
}

namespace {
std::string hashHex(const unsigned char* p,std::size_t n){auto h=sha(p,n);constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(64);for(auto b:h){out+=hex[b>>4];out+=hex[b&15];}return out;}
std::string hashHex(const Bytes& b){return hashHex(b.data(),b.size());}
void add64(Bytes& b,std::uint64_t v){putLE(b,v,8);}
void addString(Bytes& b,const std::string& s){add64(b,s.size());b.insert(b.end(),s.begin(),s.end());}
void addDouble(Bytes& b,double d){std::uint64_t bits;std::memcpy(&bits,&d,8);add64(b,bits);}
std::uint32_t number(std::string_view value,std::uint32_t maximum){
    require(!value.empty()&&value.size()<=10&&(value.size()==1||value.front()!='0'),"invalid canonical native number");for(char c:value)require(c>='0'&&c<='9',"nondecimal native number");
    std::uint32_t n=0;auto r=std::from_chars(value.data(),value.data()+value.size(),n);require(r.ec==std::errc{}&&r.ptr==value.data()+value.size()&&n<=maximum,"native number overflow");return n;
}
void validVersion(const std::string& s){textValid(s,true);std::size_t start=0;for(int i=0;i<4;++i){auto end=s.find('.',start);require(i==3?end==std::string::npos:end!=std::string::npos,"native version needs four components");if(end==std::string::npos)end=s.size();number(std::string_view(s).substr(start,end-start),UINT32_MAX);start=end+1;}}
struct SourceKey {std::uint16_t channel=0;std::string physical,pcm;};
SourceKey sourceKey(const std::string& id){
    constexpr std::string_view prefix="fl:native-channel:";require(id.compare(0,prefix.size(),prefix)==0,"source lacks native physical channel key");auto split=id.find(":pcm:",prefix.size());require(split!=std::string::npos,"source lacks PCM identity");
    SourceKey k;k.channel=static_cast<std::uint16_t>(number(std::string_view(id).substr(prefix.size(),split-prefix.size()),65535));k.physical=id.substr(0,split);k.pcm=id.substr(split+5);require(hashValid(k.pcm),"source PCM key invalid");return k;
}
std::string graphDigest(const Graph& input){
    auto g=input;auto less=[](const auto&a,const auto&b){return a.id<b.id;};std::sort(g.sources.begin(),g.sources.end(),less);std::sort(g.tracks.begin(),g.tracks.end(),less);std::sort(g.modifications.begin(),g.modifications.end(),less);std::sort(g.regions.begin(),g.regions.end(),less);Bytes b;
    addString(b,"rysup.private-fl.exact-editor-graph/1");addString(b,g.documentName);add64(b,g.sources.size());
    for(const auto&s:g.sources){addString(b,s.id);addString(b,s.name);add64(b,s.sampleCount);addDouble(b,s.sampleRate);add64(b,s.channelCount);}
    add64(b,g.tracks.size());for(const auto&t:g.tracks){addString(b,t.id);addString(b,t.name);}
    add64(b,g.modifications.size());for(const auto&m:g.modifications){addString(b,m.id);addString(b,m.sourceID);addString(b,m.name);}
    add64(b,g.regions.size());for(const auto&r:g.regions){for(const auto*s:{&r.id,&r.modificationID,&r.trackID,&r.name})addString(b,*s);for(double v:{r.sourceStart,r.sourceDuration,r.playbackStart,r.playbackDuration})addDouble(b,v);add64(b,r.transformations);}
    return hashHex(b);
}
std::string recordsDigest(const Bytes& bytes){
    require(bytes.size()%88==0&&bytes.size()/88<=maxRecords,"invalid native raw88 extent");std::vector<std::array<unsigned char,88>> records(bytes.size()/88);for(std::size_t i=0;i<records.size();++i)std::memcpy(records[i].data(),bytes.data()+i*88,88);std::sort(records.begin(),records.end());Bytes b;addString(b,"rysup.private-fl.native88-record-multiset/1");add64(b,records.size());for(const auto&r:records)b.insert(b.end(),r.begin(),r.end());return hashHex(b);
}
template<class T>T field(const Bytes& b,std::size_t at){require(at<=b.size()&&sizeof(T)<=b.size()-at,"truncated raw field");T value;std::memcpy(&value,b.data()+at,sizeof(value));return value;}
void validateGraph(const Graph& g){
    textValid(g.documentName);require(!g.regions.empty()||g.tracks.empty(),"native graph has tracks without any Playlist regions");
    require(g.sources.size()<=4096&&g.tracks.size()<=4096&&g.modifications.size()<=maxRecords&&g.regions.size()<=maxRecords&&g.sources.size()+g.tracks.size()+g.modifications.size()+g.regions.size()<=maxRecords,"graph record budget exceeded");
    std::size_t textBudget=0;auto budgetText=[&](const std::string& s){textValid(s);require(s.size()<=64u*1024u*1024u-textBudget,"graph text budget exceeded");textBudget+=s.size();};budgetText(g.documentName);
    for(const auto&s:g.sources){budgetText(s.id);budgetText(s.name);}for(const auto&t:g.tracks){budgetText(t.id);budgetText(t.name);}for(const auto&m:g.modifications){budgetText(m.id);budgetText(m.sourceID);budgetText(m.name);}for(const auto&r:g.regions){budgetText(r.id);budgetText(r.modificationID);budgetText(r.trackID);budgetText(r.name);}
    std::map<std::string,const SourceDescriptor*> sources;std::set<std::string> tracks,regions;std::map<std::string,std::string> mods;std::set<std::uint16_t> channels;std::uint64_t pcmBytes=0;
    auto text=[](const std::string& id,const std::string& name){textValid(id,true);textValid(name);};
    for(const auto&s:g.sources){text(s.id,s.name);auto key=sourceKey(s.id);require(channels.insert(key.channel).second&&sources.emplace(s.id,&s).second,"duplicate native source/channel");require(s.sampleCount>0&&s.sampleRate==48000&&(s.channelCount==1||s.channelCount==2),"source outside qualified 48k int16 mono/stereo profile");require(std::uint64_t(s.sampleCount)<=64u*1024u*1024u/(2u*s.channelCount),"source PCM extent exceeds budget");pcmBytes+=std::uint64_t(s.sampleCount)*s.channelCount*2u;require(pcmBytes<=128u*1024u*1024u,"source PCM total exceeds budget");}
    for(const auto&t:g.tracks){text(t.id,t.name);require(tracks.insert(t.id).second,"duplicate track ID");}
    for(const auto&m:g.modifications){text(m.id,m.name);textValid(m.sourceID,true);require(sources.count(m.sourceID)&&m.id==m.sourceID+":modification"&&mods.emplace(m.id,m.sourceID).second,"invalid native modification identity");}
    require(mods.size()==sources.size(),"native source/modification cardinality differs");
    for(const auto&r:g.regions){text(r.id,r.name);textValid(r.modificationID,true);textValid(r.trackID,true);require(regions.insert(r.id).second&&mods.count(r.modificationID)&&tracks.count(r.trackID),"region ID/reference invalid");for(double x:{r.sourceStart,r.sourceDuration,r.playbackStart,r.playbackDuration})require(std::isfinite(x),"nonfinite region");auto s=sources.at(mods.at(r.modificationID));double duration=double(s->sampleCount)/s->sampleRate;require(r.sourceStart>=0&&r.sourceDuration>0&&r.playbackStart>=0&&r.playbackDuration>0&&std::isfinite(r.playbackStart+r.playbackDuration)&&r.sourceStart<=duration&&r.sourceDuration<=duration-r.sourceStart+1e-9,"region outside source/timeline bounds");require(r.transformations==ARA::kARAPlaybackTransformationNoChanges&&std::abs(r.sourceDuration-r.playbackDuration)<=1e-8,"standalone native time transform unsupported");}
}
void validateAuthority(const StandaloneNativeModelAuthority& n){
    for(const auto*h:{&n.nativeEngineSHA256,&n.nativeRecordModelSHA256,&n.sourcePCMModelSHA256,&n.graphSHA256})require(hashValid(*h),"invalid native authority hash");validVersion(n.nativeSaveVersion);
    const auto& c=n.clock;textValid(c.clockFormatIdentity,true);require(c.ppq>=24&&c.ppq<=9600&&c.ppq%4==0&&c.tempoMilliBPM>=10000&&c.tempoMilliBPM<=522000&&c.sampleRate==48000&&!c.actualNativePlaying,"native clock outside qualified stopped profile");
    require(c.actualNativeClockBytes.size()==6,"native clock must contain exact u16 PPQ/u32 tempo");require(readLE(c.actualNativeClockBytes,0,2)==c.ppq&&readLE(c.actualNativeClockBytes,2,4)==c.tempoMilliBPM,"native clock typed values disagree with copied bytes");
}
struct RecordBinding {std::size_t index=0;std::int32_t clipID=0;std::string regionID,sourceID,modificationID,physical;};
std::map<std::size_t,RecordBinding> recordBindings(const Graph& g,const StandaloneNativeModelAuthority& n,const Bytes& raw){
    std::map<std::uint16_t,const SourceDescriptor*> sources;for(const auto&s:g.sources)sources.emplace(sourceKey(s.id).channel,&s);
    std::map<std::string,const RegionDescriptor*> regions;for(const auto&r:g.regions)regions.emplace(r.id,&r);
    std::set<std::string> used;std::set<std::int32_t> ids;std::map<std::size_t,RecordBinding> bindings;
    for(std::size_t at=0;at<raw.size();at+=88){auto kind=field<std::uint32_t>(raw,at+4);if(kind==0xffffffffu||static_cast<std::int32_t>(kind)>=0x50000000)continue;auto channel=field<std::uint16_t>(raw,at+6);auto source=sources.find(channel);if(source==sources.end())continue;
        const auto* s=source->second;auto clipID=field<std::int32_t>(raw,at+32);require(clipID>0&&ids.insert(clipID).second,"native selected clip ID invalid/duplicate");const auto physical="fl:native-clip:"+std::to_string(clipID),id=physical+":"+s->id+":region",mod=s->id+":modification";
        auto got=regions.find(id);require(got!=regions.end()&&used.insert(id).second,"native selected row absent/duplicated in graph");auto track=field<std::int16_t>(raw,at+12);auto ticks=field<std::int32_t>(raw,at),length=field<std::int32_t>(raw,at+8);auto lo=field<float>(raw,at+24),hi=field<float>(raw,at+28);
        require(track>=0&&ticks>=0&&length>0&&std::isfinite(lo)&&std::isfinite(hi)&&lo>=0&&hi>lo,"invalid native raw region geometry");double startFrames=double(lo)*s->sampleRate*.001,endFrames=double(hi)*s->sampleRate*.001;
        const auto&r=*got->second;double start=startFrames/s->sampleRate,duration=(endFrames-startFrames)/s->sampleRate,bpm=double(n.clock.tempoMilliBPM)/1000;
        require(r.modificationID==mod&&r.trackID=="fl:native-playlist-track:"+std::to_string(track)&&r.sourceStart==start&&r.sourceDuration==duration&&r.playbackStart==double(ticks)/n.clock.ppq*60.0/bpm&&r.playbackDuration==double(length)/n.clock.ppq*60.0/bpm,"graph geometry/identity differs from exact native records/clock");
        bindings.emplace(at/88,RecordBinding{at/88,clipID,id,s->id,mod,physical});
    }
    require(used.size()==g.regions.size(),"graph contains a region with no exact native row");return bindings;
}
void validateAudioKinds(const Bytes& raw,const std::vector<std::size_t>& indices,const std::map<std::size_t,RecordBinding>& selected){
    require(indices.size()<=raw.size()/88,"audio classification index count exceeds native records");
    std::set<std::int32_t> ids;std::size_t previous=0;bool first=true;
    for(auto index:indices){require(index<raw.size()/88&&(first||index>previous),"audio classification indices not sorted unique/bounded");first=false;previous=index;auto kind=field<std::uint32_t>(raw,index*88+4);auto id=field<std::int32_t>(raw,index*88+32);require(kind!=0xffffffffu&&static_cast<std::int32_t>(kind)<0x50000000&&id>0&&ids.insert(id).second,"verified audio row has invalid raw kind/clip ID");}
    for(const auto& entry:selected)require(std::binary_search(indices.begin(),indices.end(),entry.first),"selected graph region is not verified native AudioChannel");
}
std::string sourceModelDigest(const Graph&,const std::string&);
void validateSaved(const StandaloneSavedEditorArchive& s){
    require(s.schema==1&&s.ara.schema==1&&s.ara.nativeGraphReady,"unsupported/unready standalone archive");require(hashValid(s.pluginModuleSHA256),"invalid plugin module hash");textValid(s.factoryID,true);textValid(s.nativeInsertKey,true);require(s.factoryID==s.ara.factoryID,"ARA factory identity differs");
    for(const auto*t:{&s.ara.factoryID,&s.ara.archiveID})textValid(*t,true);for(const auto*t:{&s.ara.pluginName,&s.ara.manufacturer,&s.ara.informationURL,&s.ara.pluginVersion})textValid(*t);
    validateAuthority(s.native);validateGraph(s.ara.graph);require(s.native.sourcePCMModelSHA256==sourceModelDigest(s.ara.graph,s.nativeInsertKey),"saved source PCM model digest mismatch");require(s.native.graphSHA256==graphDigest(s.ara.graph),"native graph digest mismatch");require(s.native.nativeRecordModelSHA256==recordsDigest(s.actualCopiedNativeModelRecords),"native raw88 model digest mismatch");auto bindings=recordBindings(s.ara.graph,s.native,s.actualCopiedNativeModelRecords);validateAudioKinds(s.actualCopiedNativeModelRecords,s.actualVerifiedAudioRecordIndices,bindings);
}
Dict authorityValue(const StandaloneNativeModelAuthority& n){
    auto d=dict();setText(d,CFSTR("nativeEngineSHA256"),n.nativeEngineSHA256);setText(d,CFSTR("nativeRecordModelSHA256"),n.nativeRecordModelSHA256);setText(d,CFSTR("sourcePCMModelSHA256"),n.sourcePCMModelSHA256);setText(d,CFSTR("graphSHA256"),n.graphSHA256);setText(d,CFSTR("nativeSaveVersion"),n.nativeSaveVersion);
    auto c=dict();setInt(c,CFSTR("ppq"),n.clock.ppq);setInt(c,CFSTR("tempoMilliBPM"),n.clock.tempoMilliBPM);setReal(c,CFSTR("sampleRate"),n.clock.sampleRate);setText(c,CFSTR("clockFormatIdentity"),n.clock.clockFormatIdentity);setData(c,CFSTR("actualNativeClockBytes"),n.clock.actualNativeClockBytes);CFDictionarySetValue(d,CFSTR("clock"),c);return d;
}
StandaloneNativeModelAuthority parseAuthority(CFDictionaryRef d){
    getDict(d,6);StandaloneNativeModelAuthority n;n.nativeEngineSHA256=getText(d,CFSTR("nativeEngineSHA256"));n.nativeRecordModelSHA256=getText(d,CFSTR("nativeRecordModelSHA256"));n.sourcePCMModelSHA256=getText(d,CFSTR("sourcePCMModelSHA256"));n.graphSHA256=getText(d,CFSTR("graphSHA256"));n.nativeSaveVersion=getText(d,CFSTR("nativeSaveVersion"));
    auto c=getDict(get(d,CFSTR("clock"),CFDictionaryGetTypeID()),5);auto ppq=getInt(c,CFSTR("ppq")),tempo=getInt(c,CFSTR("tempoMilliBPM"));require(ppq>=0&&ppq<=UINT32_MAX&&tempo>=0&&tempo<=UINT32_MAX,"clock integer overflow");n.clock.ppq=ppq;n.clock.tempoMilliBPM=tempo;n.clock.sampleRate=getReal(c,CFSTR("sampleRate"));n.clock.clockFormatIdentity=getText(c,CFSTR("clockFormatIdentity"));n.clock.actualNativeClockBytes=getData(c,CFSTR("actualNativeClockBytes"));return n;
}
Dict archiveValue(const ArchiveSnapshot& s){auto d=dict();setInt(d,CFSTR("schema"),s.schema);CFDictionarySetValue(d,CFSTR("nativeGraphReady"),s.nativeGraphReady?kCFBooleanTrue:kCFBooleanFalse);setText(d,CFSTR("factoryID"),s.factoryID);setText(d,CFSTR("archiveID"),s.archiveID);setText(d,CFSTR("pluginName"),s.pluginName);setText(d,CFSTR("manufacturer"),s.manufacturer);setText(d,CFSTR("informationURL"),s.informationURL);setText(d,CFSTR("pluginVersion"),s.pluginVersion);auto g=graphValue(s.graph);CFDictionarySetValue(d,CFSTR("graph"),g);setData(d,CFSTR("araArchive"),s.araArchive);return d;}
ArchiveSnapshot parseArchive(CFDictionaryRef d){getDict(d,10);ArchiveSnapshot s;require(getInt(d,CFSTR("schema"))==1,"unsupported ARA snapshot schema");s.nativeGraphReady=CFBooleanGetValue(static_cast<CFBooleanRef>(get(d,CFSTR("nativeGraphReady"),CFBooleanGetTypeID())));s.factoryID=getText(d,CFSTR("factoryID"));s.archiveID=getText(d,CFSTR("archiveID"));s.pluginName=getText(d,CFSTR("pluginName"));s.manufacturer=getText(d,CFSTR("manufacturer"));s.informationURL=getText(d,CFSTR("informationURL"));s.pluginVersion=getText(d,CFSTR("pluginVersion"));s.graph=parseGraph(static_cast<CFDictionaryRef>(get(d,CFSTR("graph"),CFDictionaryGetTypeID())));s.araArchive=getData(d,CFSTR("araArchive"));return s;}
std::string providerPCMHash(const SourceDescriptor& descriptor,const HostController::ResolveSource& resolve){
    auto provider=resolve(descriptor);require(bool(provider),"current RAM source provider absent");auto got=provider->describe();require(got.id==descriptor.id&&got.name==descriptor.name&&got.sampleCount==descriptor.sampleCount&&got.sampleRate==descriptor.sampleRate&&got.channelCount==descriptor.channelCount,"current RAM provider identity/format mismatch");auto reader=provider->openReader(false);require(bool(reader),"current RAM source reader absent");
    constexpr std::int64_t chunk=16384;std::vector<std::vector<float>> planar(descriptor.channelCount,std::vector<float>(chunk));std::vector<void*> pointers;for(auto& channel:planar)pointers.push_back(channel.data());Bytes pcm;pcm.reserve(chunk*descriptor.channelCount*2);CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
    for(std::int64_t start=0;start<descriptor.sampleCount;start+=chunk){auto count=std::min(chunk,descriptor.sampleCount-start);for(auto& channel:planar)std::fill(channel.begin(),channel.end(),std::numeric_limits<float>::quiet_NaN());require(reader->read(start,count,pointers.data()),"actual RAM provider read failed");pcm.clear();for(std::int64_t i=0;i<count;++i)for(int channel=0;channel<descriptor.channelCount;++channel){double v=double(planar[channel][i])*32768;require(std::isfinite(v)&&v>=-32768&&v<=32767&&std::trunc(v)==v,"provider not exact admitted signed-int16 PCM");auto bits=static_cast<std::uint16_t>(static_cast<std::int16_t>(v));pcm.push_back(bits&255);pcm.push_back(bits>>8);}CC_SHA256_Update(&hash,pcm.data(),static_cast<CC_LONG>(pcm.size()));}
    std::array<unsigned char,32> digest;CC_SHA256_Final(digest.data(),&hash);constexpr char hex[]="0123456789abcdef";std::string out;for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;
}
}

void validateFLStandaloneStateEnvelope(const FLStandaloneStateEnvelope& e){
    std::size_t budget=headerBytes+4096;auto charge=[&](std::size_t n){require(n<=kFLStandaloneEnvelopeMaximumBytes-budget,"standalone envelope budget exceeded");budget+=n;};charge(e.nativeComponentState.size());charge(e.archive.ara.araArchive.size());charge(e.archive.actualCopiedNativeModelRecords.size());const auto&g=e.archive.ara.graph;
    charge((g.sources.size()+g.tracks.size()+g.modifications.size()+g.regions.size())*512);auto text=[&](const std::string&s){textValid(s);charge(s.size()*3+16);};for(const auto&s:g.sources){text(s.id);text(s.name);}for(const auto&t:g.tracks){text(t.id);text(t.name);}for(const auto&m:g.modifications){text(m.id);text(m.name);text(m.sourceID);}for(const auto&r:g.regions){text(r.id);text(r.name);text(r.modificationID);text(r.trackID);}text(g.documentName);for(const auto*s:{&e.archive.pluginModuleSHA256,&e.archive.factoryID,&e.archive.nativeInsertKey,&e.archive.native.clock.clockFormatIdentity,&e.archive.ara.archiveID,&e.archive.ara.factoryID,&e.archive.ara.pluginName,&e.archive.ara.manufacturer,&e.archive.ara.informationURL,&e.archive.ara.pluginVersion})text(*s);charge(e.archive.actualVerifiedAudioRecordIndices.size()*24);validateSaved(e.archive);
}
Bytes encodeFLStandaloneStateEnvelope(const FLStandaloneStateEnvelope& e){
    validateFLStandaloneStateEnvelope(e);auto d=dict();const auto&s=e.archive;setInt(d,CFSTR("schema"),s.schema);setText(d,CFSTR("pluginModuleSHA256"),s.pluginModuleSHA256);setText(d,CFSTR("factoryID"),s.factoryID);setText(d,CFSTR("nativeInsertKey"),s.nativeInsertKey);auto n=authorityValue(s.native),a=archiveValue(s.ara);CFDictionarySetValue(d,CFSTR("native"),n);CFDictionarySetValue(d,CFSTR("ara"),a);setData(d,CFSTR("actualCopiedNativeModelRecords"),s.actualCopiedNativeModelRecords);setData(d,CFSTR("nativeComponentState"),e.nativeComponentState);
    auto audioIndices=array();for(auto index:s.actualVerifiedAudioRecordIndices){std::int64_t i=index;CF<CFNumberRef> value(CFNumberCreate(nullptr,kCFNumberSInt64Type,&i));CFArrayAppendValue(audioIndices,value);}CFDictionarySetValue(d,CFSTR("actualVerifiedAudioRecordIndices"),audioIndices);
    CFErrorRef error=nullptr;CF<CFDataRef> payload(CFPropertyListCreateData(nullptr,d,kCFPropertyListBinaryFormat_v1_0,0,&error));CF<CFErrorRef> err(error);require(payload.get(),"standalone plist encode failed");auto length=CFDataGetLength(payload);require(length>=0&&static_cast<std::size_t>(length)<=kFLStandaloneEnvelopeMaximumBytes-headerBytes,"standalone encoded payload too large");auto raw=CFDataGetBytePtr(payload);auto h=sha(raw,length);Bytes out;out.reserve(headerBytes+length);out.insert(out.end(),magic,magic+8);putLE(out,1,4);putLE(out,length,8);out.insert(out.end(),h.begin(),h.end());out.insert(out.end(),raw,raw+length);return out;
}
bool hasFLStandaloneStateEnvelopeMagic(const Bytes& b)noexcept{return b.size()>=8&&std::memcmp(b.data(),magic,8)==0;}
std::optional<FLStandaloneStateEnvelope> decodeFLStandaloneStateEnvelope(const Bytes& b){
    if(!hasFLStandaloneStateEnvelopeMagic(b))return {};require(b.size()>=headerBytes&&b.size()<=kFLStandaloneEnvelopeMaximumBytes,"standalone envelope truncated/oversized");require(readLE(b,8,4)==1,"standalone version unsupported");auto length=readLE(b,12,8);require(length==b.size()-headerBytes,"standalone length mismatch");auto h=sha(b.data()+headerBytes,length);require(std::equal(h.begin(),h.end(),b.begin()+20),"standalone checksum mismatch");require(length>=8&&std::memcmp(b.data()+headerBytes,"bplist00",8)==0,"standalone payload not binary plist");
    CF<CFDataRef> data(CFDataCreate(nullptr,b.data()+headerBytes,length));CFErrorRef error=nullptr;CFPropertyListFormat format{};CF<CFPropertyListRef> plist(CFPropertyListCreateWithData(nullptr,data,kCFPropertyListImmutable,&format,&error));CF<CFErrorRef> err(error);require(plist.get()&&format==kCFPropertyListBinaryFormat_v1_0,"invalid standalone binary plist");auto d=getDict(plist,9);FLStandaloneStateEnvelope e;require(getInt(d,CFSTR("schema"))==1,"standalone archive schema unsupported");auto&s=e.archive;s.pluginModuleSHA256=getText(d,CFSTR("pluginModuleSHA256"));s.factoryID=getText(d,CFSTR("factoryID"));s.nativeInsertKey=getText(d,CFSTR("nativeInsertKey"));s.native=parseAuthority(static_cast<CFDictionaryRef>(get(d,CFSTR("native"),CFDictionaryGetTypeID())));s.ara=parseArchive(static_cast<CFDictionaryRef>(get(d,CFSTR("ara"),CFDictionaryGetTypeID())));s.actualCopiedNativeModelRecords=getData(d,CFSTR("actualCopiedNativeModelRecords"));e.nativeComponentState=getData(d,CFSTR("nativeComponentState"));
    auto indices=getArray(d,CFSTR("actualVerifiedAudioRecordIndices"));for(CFIndex i=0;i<CFArrayGetCount(indices);++i){auto raw=CFArrayGetValueAtIndex(indices,i);require(CFGetTypeID(raw)==CFNumberGetTypeID()&&!CFNumberIsFloatType(static_cast<CFNumberRef>(raw)),"audio index must be integer");std::int64_t index=-1;require(CFNumberGetValue(static_cast<CFNumberRef>(raw),kCFNumberSInt64Type,&index)&&index>=0&&index<static_cast<std::int64_t>(maxRecords),"audio index out of range");s.actualVerifiedAudioRecordIndices.push_back(index);}
    validateFLStandaloneStateEnvelope(e);return e;
}
namespace {
std::uint32_t mixerInsert(const std::string& key){constexpr std::string_view prefix="fl:native-mixer-insert:";require(key.compare(0,prefix.size(),prefix)==0,"native insert key has unsupported grammar");return number(std::string_view(key).substr(prefix.size()),500);}
std::string sourceModelDigest(const Graph& input,const std::string& insert){auto sources=input.sources;std::sort(sources.begin(),sources.end(),[](const auto&a,const auto&b){return a.id<b.id;});Bytes b;addString(b,"rysup.standalone-fl.native-channel-copied-pcm/1");auto mixer=mixerInsert(insert);add64(b,mixer);add64(b,sources.size());for(const auto&s:sources){auto key=sourceKey(s.id);addString(b,s.id);add64(b,key.channel);add64(b,mixer);add64(b,s.sampleCount);add64(b,s.channelCount);addDouble(b,s.sampleRate);addDouble(b,1.0);add64(b,s.channelCount);add64(b,0);addString(b,key.pcm);}return hashHex(b);}
bool sameClock(const StandaloneNativeClock&a,const StandaloneNativeClock&b){return a.ppq==b.ppq&&a.tempoMilliBPM==b.tempoMilliBPM&&a.sampleRate==b.sampleRate&&a.clockFormatIdentity==b.clockFormatIdentity&&a.actualNativeClockBytes==b.actualNativeClockBytes;}
}
FLStandaloneRestoreGraphValidation validateFLStandaloneRestoreGraph(const StandaloneSavedEditorArchive& saved,const StandaloneSnapshotData& current){
    validateSaved(saved);validateAuthority(current.model);validateGraph(current.graph);
    require(current.actualCurrentNativeRAMCopiedAndVerified&&current.actualDirectNativeClockReadTwiceAndEqual&&current.nativeComponentOwnerInsertAssociationVerified&&current.noNativeSerializationOrFileFallbackUsed,"current native RAM/clock/owner proof missing");
    require(current.actual.actualComponent&&current.actual.actualNativeEngineOwner&&!current.actual.instanceID.empty()&&!current.decoderIdentity.empty()&&hashValid(current.decoderSourceSHA256),"current exact native association/decoder missing");
    require(saved.pluginModuleSHA256==current.actual.pluginModuleSHA256&&saved.nativeInsertKey==current.actual.nativeInsertKey&&saved.native.nativeEngineSHA256==current.actual.nativeEngineSHA256&&saved.native.nativeEngineSHA256==current.model.nativeEngineSHA256&&saved.native.nativeSaveVersion==current.model.nativeSaveVersion&&sameClock(saved.native.clock,current.model.clock),"native plugin/insert/engine/version/clock differs");
    require(saved.native.sourcePCMModelSHA256==sourceModelDigest(saved.ara.graph,saved.nativeInsertKey)&&current.model.sourcePCMModelSHA256==sourceModelDigest(current.graph,current.actual.nativeInsertKey)&&saved.native.sourcePCMModelSHA256==current.model.sourcePCMModelSHA256,"source PCM/channel/format/insert model differs");
    require(current.model.nativeRecordModelSHA256==recordsDigest(current.actualCopiedNativeModelRecords)&&current.model.graphSHA256==graphDigest(current.graph),"current record/graph authority digest invalid");
    require(saved.actualCopiedNativeModelRecords.size()==current.actualCopiedNativeModelRecords.size(),"native raw88 extent changed");
    auto oldBindings=recordBindings(saved.ara.graph,saved.native,saved.actualCopiedNativeModelRecords),newBindings=recordBindings(current.graph,current.model,current.actualCopiedNativeModelRecords);require(oldBindings.size()==newBindings.size(),"native selected region count changed");
    bool relocated=false;std::set<std::int32_t> oldIDs,newIDs;
    require(saved.actualVerifiedAudioRecordIndices==current.actualVerifiedAudioRecordIndices,"native actual audio-row classifications changed");
    validateAudioKinds(current.actualCopiedNativeModelRecords,current.actualVerifiedAudioRecordIndices,newBindings);
    // ONLY explicitly native-kind-qualified audio rows may regenerate clip IDs.
    // Non-audio rows preserve every byte, including +0x20.
    for(std::size_t index=0;index<saved.actualCopiedNativeModelRecords.size()/88;++index){const auto at=index*88;bool selected=std::binary_search(saved.actualVerifiedAudioRecordIndices.begin(),saved.actualVerifiedAudioRecordIndices.end(),index);require(bool(oldBindings.count(index))==bool(newBindings.count(index)),"selected native row changed");for(std::size_t offset=0;offset<88;++offset){if(selected&&offset>=32&&offset<36)continue;require(saved.actualCopiedNativeModelRecords[at+offset]==current.actualCopiedNativeModelRecords[at+offset],"native raw88 non-ID byte/order changed");}
        if(selected){auto old=field<std::int32_t>(saved.actualCopiedNativeModelRecords,at+32),now=field<std::int32_t>(current.actualCopiedNativeModelRecords,at+32);require(old>0&&now>0&&oldIDs.insert(old).second&&newIDs.insert(now).second,"native regenerated clip IDs not bijective");relocated|=old!=now;}}
    require((current.graph.sources.empty()||current.resolveCurrentImmutableRAMSource)&&current.sources.size()==current.graph.sources.size()&&current.modifications.size()==current.graph.modifications.size()&&current.regions.size()==current.graph.regions.size(),"current native physical identity counts/resolver differ");
    std::map<std::string,const StandaloneSourceIdentity*> sourceIDs;for(const auto&s:current.sources)require(sourceIDs.emplace(s.sourceID,&s).second,"duplicate actual source identity");
    for(const auto&s:current.graph.sources){auto key=sourceKey(s.id);auto got=sourceIDs.find(s.id);require(got!=sourceIDs.end()&&got->second->nativePhysicalSourceKey==key.physical&&got->second->actualPCM_SHA256==key.pcm,"actual native source physical/PCM identity differs");require(providerPCMHash(s,current.resolveCurrentImmutableRAMSource)==key.pcm,"current provider PCM differs from native source identity");}
    std::map<std::string,const StandaloneModificationIdentity*> modifications;for(const auto&m:current.modifications)require(modifications.emplace(m.modificationID,&m).second,"duplicate actual modification identity");
    for(const auto&m:current.graph.modifications){auto key=sourceKey(m.sourceID);auto got=modifications.find(m.id);require(got!=modifications.end()&&got->second->sourceID==m.sourceID&&got->second->nativePhysicalEditKey==key.physical,"actual native edit/source identity differs");}
    std::map<std::string,const StandaloneRegionIdentity*> regions;for(const auto&r:current.regions)require(regions.emplace(r.regionID,&r).second,"duplicate actual region identity");
    for(const auto&[index,b]:newBindings){auto got=regions.find(b.regionID);require(got!=regions.end()&&got->second->modificationID==b.modificationID&&got->second->nativePhysicalRegionKey==b.physical,"actual native region physical identity differs");auto old=oldBindings.find(index);require(old!=oldBindings.end()&&old->second.sourceID==b.sourceID&&old->second.modificationID==b.modificationID,"native source/edit reassignment during ID regeneration");}
    auto oldGraph=saved.ara.graph,newGraph=current.graph;auto sort=[](auto& rows){std::sort(rows.begin(),rows.end(),[](const auto&a,const auto&b){return a.id<b.id;});};sort(oldGraph.sources);sort(newGraph.sources);sort(oldGraph.modifications);sort(newGraph.modifications);sort(oldGraph.tracks);sort(newGraph.tracks);
    require(oldGraph.documentName==newGraph.documentName&&oldGraph.sources.size()==newGraph.sources.size()&&oldGraph.modifications.size()==newGraph.modifications.size()&&oldGraph.tracks.size()==newGraph.tracks.size(),"physical graph metadata/cardinality changed");
    ArchiveObjectIDRelocation mapping;
    for(std::size_t i=0;i<oldGraph.sources.size();++i){const auto&a=oldGraph.sources[i];const auto&b=newGraph.sources[i];require(a.id==b.id&&a.name==b.name&&a.sampleCount==b.sampleCount&&a.sampleRate==b.sampleRate&&a.channelCount==b.channelCount,"native source graph changed");mapping.sourceIDs.emplace_back(a.id,b.id);}
    for(std::size_t i=0;i<oldGraph.modifications.size();++i){const auto&a=oldGraph.modifications[i];const auto&b=newGraph.modifications[i];require(a.id==b.id&&a.sourceID==b.sourceID&&a.name==b.name,"native modification graph changed");mapping.modificationIDs.emplace_back(a.id,b.id);}
    for(std::size_t i=0;i<oldGraph.tracks.size();++i)require(oldGraph.tracks[i].id==newGraph.tracks[i].id&&oldGraph.tracks[i].name==newGraph.tracks[i].name,"native track graph changed");
    std::map<std::string,const RegionDescriptor*> oldRegions,newRegions;for(const auto&r:oldGraph.regions)oldRegions.emplace(r.id,&r);for(const auto&r:newGraph.regions)newRegions.emplace(r.id,&r);
    for(const auto&[index,b]:oldBindings){const auto&now=newBindings.at(index);const auto&a=*oldRegions.at(b.regionID);const auto&c=*newRegions.at(now.regionID);require(a.modificationID==c.modificationID&&a.trackID==c.trackID&&a.name==c.name&&a.sourceStart==c.sourceStart&&a.sourceDuration==c.sourceDuration&&a.playbackStart==c.playbackStart&&a.playbackDuration==c.playbackDuration&&a.transformations==c.transformations,"native region physical geometry changed");mapping.regionIDs.emplace_back(a.id,c.id);}
    FLStandaloneRestoreGraphValidation result;result.current=current;result.objectIDRelocationVerified=relocated;result.verifiedOldIncomingNativeModel=saved.native;
    if(relocated)result.objectIDs=std::move(mapping);else require(saved.native.nativeRecordModelSHA256==current.model.nativeRecordModelSHA256&&saved.native.graphSHA256==current.model.graphSHA256,"nonrelocated model changed");return result;
}

namespace fl_app {
MemoryIBStream::MemoryIBStream(Bytes b):data_(std::move(b)){require(data_.size()<=kFLStandaloneEnvelopeMaximumBytes,"memory stream exceeds limit");}
Steinberg::tresult PLUGIN_API MemoryIBStream::queryInterface(const Steinberg::TUID iid,void** out){
    if(!out)return Steinberg::kInvalidArgument;*out=nullptr;
    if(Steinberg::FUnknownPrivate::iidEqual(iid,INLINE_UID_OF(Steinberg::IBStream))||Steinberg::FUnknownPrivate::iidEqual(iid,INLINE_UID_OF(Steinberg::FUnknown)))*out=static_cast<Steinberg::IBStream*>(this);
    else if(Steinberg::FUnknownPrivate::iidEqual(iid,INLINE_UID_OF(Steinberg::ISizeableStream)))*out=static_cast<Steinberg::ISizeableStream*>(this);
    else return Steinberg::kNoInterface;
    addRef();return Steinberg::kResultOk;
}
Steinberg::uint32 PLUGIN_API MemoryIBStream::addRef(){return ++refs_;}
Steinberg::uint32 PLUGIN_API MemoryIBStream::release(){auto n=--refs_;if(!n)delete this;return n;}
Steinberg::tresult PLUGIN_API MemoryIBStream::read(void* out,Steinberg::int32 count,Steinberg::int32* got){
    if(got)*got=0;if(count<0||(!out&&count>0))return Steinberg::kInvalidArgument;if(!count)return Steinberg::kResultOk;
    const auto available=position_>=static_cast<Steinberg::int64>(data_.size())?0:static_cast<Steinberg::int64>(data_.size())-position_;
    auto n=std::min<std::int64_t>(count,available);if(n)std::memcpy(out,data_.data()+position_,n);position_+=n;if(got)*got=static_cast<Steinberg::int32>(n);return n?Steinberg::kResultOk:Steinberg::kResultFalse;
}
Steinberg::tresult PLUGIN_API MemoryIBStream::write(void* in,Steinberg::int32 count,Steinberg::int32* put){
    if(put)*put=0;if(count<0||(!in&&count>0)||static_cast<std::uint64_t>(count)>kFLStandaloneEnvelopeMaximumBytes-position_)return Steinberg::kInvalidArgument;
    try{if(count){data_.resize(std::max(data_.size(),static_cast<std::size_t>(position_+count)));std::memcpy(data_.data()+position_,in,count);position_+=count;}if(put)*put=count;return Steinberg::kResultOk;}catch(...){return Steinberg::kOutOfMemory;}
}
Steinberg::tresult PLUGIN_API MemoryIBStream::seek(Steinberg::int64 offset,Steinberg::int32 mode,Steinberg::int64* result){
    std::int64_t base=0;if(mode==Steinberg::IBStream::kIBSeekCur)base=position_;else if(mode==Steinberg::IBStream::kIBSeekEnd)base=data_.size();else if(mode!=Steinberg::IBStream::kIBSeekSet)return Steinberg::kInvalidArgument;
    if(offset < -base || offset > static_cast<std::int64_t>(kFLStandaloneEnvelopeMaximumBytes)-base)return Steinberg::kInvalidArgument;
    position_=base+offset;if(result)*result=position_;return Steinberg::kResultOk;
}
Steinberg::tresult PLUGIN_API MemoryIBStream::tell(Steinberg::int64* out){if(!out)return Steinberg::kInvalidArgument;*out=position_;return Steinberg::kResultOk;}
Steinberg::tresult PLUGIN_API MemoryIBStream::getStreamSize(Steinberg::int64& n){n=data_.size();return Steinberg::kResultOk;}
Steinberg::tresult PLUGIN_API MemoryIBStream::setStreamSize(Steinberg::int64 n){if(n<0||n>static_cast<std::int64_t>(kFLStandaloneEnvelopeMaximumBytes))return Steinberg::kInvalidArgument;try{data_.resize(n);return Steinberg::kResultOk;}catch(...){return Steinberg::kOutOfMemory;}}
Bytes readRemaining(Steinberg::IBStream& stream,std::size_t maximum){
    require(maximum<=kFLStandaloneEnvelopeMaximumBytes,"invalid stream limit");Bytes b;std::array<unsigned char,65536> scratch{};
    for(;;){const auto want=static_cast<Steinberg::int32>(std::min(scratch.size(),maximum-b.size()+1));Steinberg::int32 n=0;const auto status=stream.read(scratch.data(),want,&n);
        require(n>=0&&n<=want,"invalid stream read count");require(status==Steinberg::kResultOk||status==Steinberg::kResultFalse,"stream read failed");require(static_cast<std::size_t>(n)<=maximum-b.size(),"stream exceeds limit");b.insert(b.end(),scratch.begin(),scratch.begin()+n);if(!n)return b;}
}
void writeAll(Steinberg::IBStream& stream,const Bytes& b){
    require(b.size()<=kFLStandaloneEnvelopeMaximumBytes,"write exceeds limit");std::size_t pos=0;
    while(pos<b.size()){auto want=static_cast<Steinberg::int32>(std::min<std::size_t>(65536,b.size()-pos));Steinberg::int32 n=0;auto status=stream.write(const_cast<ARA::ARAByte*>(b.data()+pos),want,&n);require(status==Steinberg::kResultOk&&n>0&&n<=want,"stream write failed/no progress");pos+=n;}
}
bool peekMagicAndRestorePosition(Steinberg::IBStream& stream){
    Steinberg::int64 position=0;require(stream.tell(&position)==Steinberg::kResultOk && position>=0,"stream tell failed");
    auto restore=[&]{Steinberg::int64 at=-1;require(stream.seek(position,Steinberg::IBStream::kIBSeekSet,&at)==Steinberg::kResultOk&&at==position,"stream position restore failed");};
    bool result=false;
    try{Bytes b(8);std::size_t pos=0;while(pos<8){Steinberg::int32 got=0;auto status=stream.read(b.data()+pos,static_cast<Steinberg::int32>(8-pos),&got);require((status==Steinberg::kResultOk||status==Steinberg::kResultFalse)&&got>=0&&got<=static_cast<Steinberg::int32>(8-pos),"magic peek read failed");if(!got)break;pos+=got;}b.resize(pos);result=hasFLStandaloneStateEnvelopeMagic(b);}catch(...){restore();throw;}
    restore();return result;
}

} // namespace fl_app
} // namespace rysup::ara_lab

#ifdef RYSUP_FL_STANDALONE_ENVELOPE_SELF_TEST
#include <iostream>
#include <functional>
namespace {
using namespace rysup::ara_lab;
unsigned checks=0;
void check(bool good,const char* message){if(!good)throw std::runtime_error(message);++checks;}
void rejects(const std::function<void()>& f){bool refused=false;try{f();}catch(const std::exception&){refused=true;}check(refused,"negative control unexpectedly accepted");}
template<class T>void writeField(Bytes& b,std::size_t at,T value){std::memcpy(b.data()+at,&value,sizeof(value));}
class OfflinePCM final:public SourceProvider {
public:
    SourceDescriptor descriptor;std::shared_ptr<const Bytes> pcm;
    OfflinePCM(SourceDescriptor d,Bytes b):descriptor(std::move(d)),pcm(std::make_shared<const Bytes>(std::move(b))){}
    SourceDescriptor describe()const override{return descriptor;}
    class Reader final:public SourceReader {
    public:std::shared_ptr<const Bytes> pcm;SourceDescriptor d;bool wide;
        Reader(std::shared_ptr<const Bytes> p,SourceDescriptor s,bool w):pcm(std::move(p)),d(std::move(s)),wide(w){}
        bool read(std::int64_t start,std::int64_t frames,void*const channels[])noexcept override{if(!channels||start<0||frames<0||start+frames>d.sampleCount)return false;for(std::int64_t i=0;i<frames;++i)for(int c=0;c<d.channelCount;++c){std::size_t at=(std::size_t(start+i)*d.channelCount+c)*2;std::uint16_t bits=(*pcm)[at]|(std::uint16_t((*pcm)[at+1])<<8);std::int16_t v;std::memcpy(&v,&bits,2);if(wide)static_cast<double*>(channels[c])[i]=double(v)/32768;else static_cast<float*>(channels[c])[i]=float(v)/32768;}return true;}
    };
    std::unique_ptr<SourceReader> openReader(bool wide)override{return std::make_unique<Reader>(pcm,descriptor,wide);}
};
struct Fixture {FLStandaloneStateEnvelope envelope;StandaloneSnapshotData current;Bytes pcm;};
void seal(StandaloneSnapshotData& d){d.model.graphSHA256=graphDigest(d.graph);d.model.nativeRecordModelSHA256=recordsDigest(d.actualCopiedNativeModelRecords);d.model.sourcePCMModelSHA256=sourceModelDigest(d.graph,d.actual.nativeInsertKey);}
Fixture fixture(){
    Fixture f;f.pcm.resize(96000*2);for(std::size_t i=0;i<96000;++i){auto sample=static_cast<std::int16_t>((i%2000)-1000);writeField(f.pcm,i*2,sample);}auto pcmSHA=hashHex(f.pcm);const std::string physical="fl:native-channel:0",source=physical+":pcm:"+pcmSHA,mod=source+":modification",track="fl:native-playlist-track:0";
    auto& d=f.current;d.actual.instanceID="offline-component";d.actual.actualComponent=1;d.actual.actualNativeEngineOwner=2;d.actual.nativeInsertKey="fl:native-mixer-insert:1";d.actual.nativeEngineSHA256=std::string(64,'a');d.actual.pluginModuleSHA256=std::string(64,'b');d.decoderIdentity="offline-synthetic-raw88";d.decoderSourceSHA256=std::string(64,'c');d.actualCurrentNativeRAMCopiedAndVerified=d.actualDirectNativeClockReadTwiceAndEqual=d.nativeComponentOwnerInsertAssociationVerified=d.noNativeSerializationOrFileFallbackUsed=true;
    d.graph.documentName="FL ARA";d.graph.sources={{source,"",96000,48000,1}};d.graph.tracks={{track,""}};d.graph.modifications={{mod,source,""}};
    d.graph.regions={{"fl:native-clip:10:"+source+":region",mod,track,"",0,2,0,2,0},{"fl:native-clip:20:"+source+":region",mod,track,"",.25,1,3,1,0}};
    d.sources={{source,physical,pcmSHA}};d.modifications={{mod,physical,source}};d.regions={{d.graph.regions[0].id,"fl:native-clip:10",mod},{d.graph.regions[1].id,"fl:native-clip:20",mod}};
    d.actualCopiedNativeModelRecords.resize(88*4);auto row=[&](std::size_t i,std::uint16_t channel,std::int32_t id,std::int32_t start,std::int32_t length,float lo,float hi){auto at=i*88;writeField(d.actualCopiedNativeModelRecords,at,start);writeField(d.actualCopiedNativeModelRecords,at+4,std::uint32_t(channel)<<16);writeField(d.actualCopiedNativeModelRecords,at+8,length);writeField(d.actualCopiedNativeModelRecords,at+12,std::int16_t(0));writeField(d.actualCopiedNativeModelRecords,at+24,lo);writeField(d.actualCopiedNativeModelRecords,at+28,hi);writeField(d.actualCopiedNativeModelRecords,at+32,id);};
    row(0,0,10,0,3840,0,2000);row(1,0,20,5760,1920,250,1250);row(2,1,30,0,3840,0,2000);row(3,2,40,0,3840,0,2000);writeField(d.actualCopiedNativeModelRecords,3*88+4,std::uint32_t(0x50000000));d.actualVerifiedAudioRecordIndices={0,1,2};
    d.model.nativeProjectIdentity="offline-runtime-project-not-persisted";d.model.nativeEngineSHA256=d.actual.nativeEngineSHA256;d.model.nativeSaveVersion="26.1.7.5421";auto& c=d.model.clock;c.ppq=960;c.tempoMilliBPM=120000;c.sampleRate=48000;c.clockFormatIdentity="fl-direct-ppq-u16-tempo-u32-le/1";putLE(c.actualNativeClockBytes,c.ppq,2);putLE(c.actualNativeClockBytes,c.tempoMilliBPM,4);seal(d);
    auto provider=std::make_shared<OfflinePCM>(d.graph.sources[0],f.pcm);d.resolveCurrentImmutableRAMSource=[provider](const SourceDescriptor&){return provider;};
    auto& s=f.envelope.archive;s.pluginModuleSHA256=d.actual.pluginModuleSHA256;s.factoryID="offline-factory";s.nativeInsertKey=d.actual.nativeInsertKey;s.native=d.model;s.actualCopiedNativeModelRecords=d.actualCopiedNativeModelRecords;s.actualVerifiedAudioRecordIndices=d.actualVerifiedAudioRecordIndices;s.ara.factoryID=s.factoryID;s.ara.archiveID="offline-dummy-archive";s.ara.pluginName="Offline codec only";s.ara.nativeGraphReady=true;s.ara.graph=d.graph;s.ara.araArchive={0,1,2,255};f.envelope.nativeComponentState={3,0,4,255};return f;
}
StandaloneSnapshotData regenerate(StandaloneSnapshotData d){for(std::size_t i=0;i<3;++i){auto old=field<std::int32_t>(d.actualCopiedNativeModelRecords,i*88+32),now=old+100;writeField(d.actualCopiedNativeModelRecords,i*88+32,now);if(i<2){const auto oldPhysical="fl:native-clip:"+std::to_string(old),newPhysical="fl:native-clip:"+std::to_string(now);d.graph.regions[i].id.replace(0,oldPhysical.size(),newPhysical);d.regions[i].regionID=d.graph.regions[i].id;d.regions[i].nativePhysicalRegionKey=newPhysical;}}seal(d);return d;}
struct Release{void operator()(fl_app::MemoryIBStream*p)const{p->release();}};using Stream=std::unique_ptr<fl_app::MemoryIBStream,Release>;
Bytes mutatePlist(const Bytes& bytes,const std::function<void(CFMutableDictionaryRef)>& mutation){CF<CFDataRef> b(CFDataCreate(nullptr,bytes.data()+52,bytes.size()-52));CF<CFPropertyListRef> p(CFPropertyListCreateWithData(nullptr,b,kCFPropertyListMutableContainersAndLeaves,nullptr,nullptr));mutation(static_cast<CFMutableDictionaryRef>(const_cast<void*>(p.get())));CF<CFDataRef> encoded(CFPropertyListCreateData(nullptr,p,kCFPropertyListBinaryFormat_v1_0,0,nullptr));auto raw=CFDataGetBytePtr(encoded);auto size=CFDataGetLength(encoded);Bytes out;out.insert(out.end(),magic,magic+8);putLE(out,1,4);putLE(out,size,8);auto h=sha(raw,size);out.insert(out.end(),h.begin(),h.end());out.insert(out.end(),raw,raw+size);return out;}
}
int main(){
    using namespace rysup::ara_lab;using namespace Steinberg;
    try{
        auto f=fixture();auto wire=encodeFLStandaloneStateEnvelope(f.envelope);auto read=decodeFLStandaloneStateEnvelope(wire);
        check(read&&read->nativeComponentState==f.envelope.nativeComponentState&&read->archive.ara.araArchive==f.envelope.archive.ara.araArchive,"opaque native/archive bytes exact");
        check(read->archive.actualCopiedNativeModelRecords==f.current.actualCopiedNativeModelRecords&&read->archive.actualVerifiedAudioRecordIndices==f.current.actualVerifiedAudioRecordIndices,"native raw/classification exact");
        check(read->archive.native.nativeProjectIdentity.empty()&&read->archive.native.clock.nativeSamplePosition==0,"runtime project/transport identity not serialized");check(encodeFLStandaloneStateEnvelope(*read)==wire,"canonical codec roundtrip");
        // Last-clip deletion does not imply source/modification deletion. The
        // immutable source still exists in the actual native channel manager.
        auto noRegion=f.current;noRegion.graph.regions.clear();noRegion.graph.tracks.clear();noRegion.regions.clear();
        noRegion.actualCopiedNativeModelRecords=Bytes(f.current.actualCopiedNativeModelRecords.begin()+176,f.current.actualCopiedNativeModelRecords.end());noRegion.actualVerifiedAudioRecordIndices={0};seal(noRegion);
        auto savedFor=[&](const StandaloneSnapshotData& data){auto saved=f.envelope;saved.archive.ara.graph=data.graph;saved.archive.native=data.model;saved.archive.actualCopiedNativeModelRecords=data.actualCopiedNativeModelRecords;saved.archive.actualVerifiedAudioRecordIndices=data.actualVerifiedAudioRecordIndices;return saved;};
        auto orphanSaved=savedFor(noRegion);auto orphanRead=decodeFLStandaloneStateEnvelope(encodeFLStandaloneStateEnvelope(orphanSaved));
        check(orphanRead&&orphanRead->archive.ara.graph.sources.size()==1&&orphanRead->archive.ara.graph.modifications.size()==1&&orphanRead->archive.ara.graph.regions.empty()&&orphanRead->archive.ara.araArchive==f.envelope.archive.ara.araArchive,"orphan source/mod retains exact real archive bytes after last-region deletion");
        auto orphanValidated=validateFLStandaloneRestoreGraph(orphanRead->archive,noRegion);check(!orphanValidated.objectIDRelocationVerified,"orphan source current native PCM remains verified without regions");
        auto orphanChanged=noRegion;writeField(orphanChanged.actualCopiedNativeModelRecords,32,std::int32_t(130));seal(orphanChanged);
        auto orphanRelocated=validateFLStandaloneRestoreGraph(orphanRead->archive,orphanChanged);check(orphanRelocated.objectIDRelocationVerified&&orphanRelocated.objectIDs.sourceIDs.size()==1&&orphanRelocated.objectIDs.modificationIDs.size()==1&&orphanRelocated.objectIDs.regionIDs.empty(),"other-insert ID regeneration preserves orphan source/mod archive mapping");
        auto empty=noRegion;empty.graph.sources.clear();empty.graph.modifications.clear();empty.sources.clear();empty.modifications.clear();empty.resolveCurrentImmutableRAMSource={};seal(empty);
        auto emptyRead=decodeFLStandaloneStateEnvelope(encodeFLStandaloneStateEnvelope(savedFor(empty)));check(emptyRead&&emptyRead->archive.ara.graph.sources.empty()&&emptyRead->archive.ara.araArchive==f.envelope.archive.ara.araArchive,"true empty selected insert preserves genuine archive payload");
        check(!validateFLStandaloneRestoreGraph(emptyRead->archive,empty).objectIDRelocationVerified,"empty selected insert validates remaining otherinsert records");
        auto noRows=empty;noRows.actualCopiedNativeModelRecords.clear();noRows.actualVerifiedAudioRecordIndices.clear();seal(noRows);auto noRowsRead=decodeFLStandaloneStateEnvelope(encodeFLStandaloneStateEnvelope(savedFor(noRows)));
        check(noRowsRead&&noRowsRead->archive.actualCopiedNativeModelRecords.empty()&&!validateFLStandaloneRestoreGraph(noRowsRead->archive,noRows).objectIDRelocationVerified,"zero native Playlist rows have exact sealed empty record/graph/source digests");
        auto wrongOrphan=noRegion;wrongOrphan.resolveCurrentImmutableRAMSource={};rejects([&]{validateFLStandaloneRestoreGraph(orphanRead->archive,wrongOrphan);});
        auto dangling=orphanSaved;dangling.archive.ara.graph.modifications.clear();dangling.archive.native.graphSHA256=graphDigest(dangling.archive.ara.graph);rejects([&]{encodeFLStandaloneStateEnvelope(dangling);});
        dangling=orphanSaved;dangling.archive.ara.graph.sources.clear();dangling.archive.native.graphSHA256=graphDigest(dangling.archive.ara.graph);rejects([&]{encodeFLStandaloneStateEnvelope(dangling);});
        dangling=orphanSaved;dangling.archive.ara.graph.tracks={{"fl:native-playlist-track:0",""}};dangling.archive.native.graphSHA256=graphDigest(dangling.archive.ara.graph);rejects([&]{encodeFLStandaloneStateEnvelope(dangling);});
        dangling=savedFor(noRows);dangling.archive.ara.nativeGraphReady=false;rejects([&]{encodeFLStandaloneStateEnvelope(dangling);});
        auto missingRegion=f.envelope;missingRegion.archive.ara.graph.regions.clear();missingRegion.archive.ara.graph.tracks.clear();missingRegion.archive.native.graphSHA256=graphDigest(missingRegion.archive.ara.graph);rejects([&]{encodeFLStandaloneStateEnvelope(missingRegion);});
        auto same=validateFLStandaloneRestoreGraph(read->archive,f.current);check(!same.objectIDRelocationVerified&&same.objectIDs.regionIDs.empty(),"identical current native graph admitted without fake relocation");
        auto regenerated=regenerate(f.current);auto moved=validateFLStandaloneRestoreGraph(read->archive,regenerated);
        check(moved.objectIDRelocationVerified&&moved.objectIDs.sourceIDs.size()==1&&moved.objectIDs.modificationIDs.size()==1&&moved.objectIDs.regionIDs.size()==2,"full bounded ID maps after selected/otherinsert audio renumber");
        check(moved.objectIDs.sourceIDs[0].first==moved.objectIDs.sourceIDs[0].second&&moved.objectIDs.modificationIDs[0].first==moved.objectIDs.modificationIDs[0].second&&moved.objectIDs.regionIDs[0].first!=moved.objectIDs.regionIDs[0].second,"source/mod stable region IDs regenerated");
        check(moved.verifiedOldIncomingNativeModel.nativeRecordModelSHA256==read->archive.native.nativeRecordModelSHA256,"old verified saved raw authority retained");
        auto otherOnly=f.current;writeField(otherOnly.actualCopiedNativeModelRecords,2*88+32,std::int32_t(130));seal(otherOnly);auto otherResult=validateFLStandaloneRestoreGraph(read->archive,otherOnly);check(otherResult.objectIDRelocationVerified&&otherResult.objectIDs.regionIDs[0].first==otherResult.objectIDs.regionIDs[0].second,"otherinsert-only audio renumber uses explicit identity mapping");
        auto rejectCurrent=[&](const std::function<void(StandaloneSnapshotData&)>& mutation,bool reseal=false){auto d=f.current;mutation(d);if(reseal)seal(d);rejects([&]{validateFLStandaloneRestoreGraph(read->archive,d);});};
        rejectCurrent([](auto&d){d.actualCurrentNativeRAMCopiedAndVerified=false;});rejectCurrent([](auto&d){d.actualDirectNativeClockReadTwiceAndEqual=false;});rejectCurrent([](auto&d){d.nativeComponentOwnerInsertAssociationVerified=false;});rejectCurrent([](auto&d){d.noNativeSerializationOrFileFallbackUsed=false;});
        rejectCurrent([](auto&d){d.actual.pluginModuleSHA256=std::string(64,'d');});rejectCurrent([](auto&d){d.actual.nativeInsertKey="fl:native-mixer-insert:2";});rejectCurrent([](auto&d){d.actual.nativeEngineSHA256=std::string(64,'e');});rejectCurrent([](auto&d){d.model.nativeSaveVersion="26.1.8.1";});
        rejectCurrent([](auto&d){d.model.clock.ppq=1920;d.model.clock.actualNativeClockBytes.clear();putLE(d.model.clock.actualNativeClockBytes,1920,2);putLE(d.model.clock.actualNativeClockBytes,120000,4);});
        rejectCurrent([](auto&d){d.model.clock.actualNativeClockBytes[0]^=1;});rejectCurrent([](auto&d){d.model.clock.actualNativePlaying=true;});
        rejectCurrent([](auto&d){d.sources[0].nativePhysicalSourceKey="fl:native-channel:1";});rejectCurrent([](auto&d){d.sources[0].actualPCM_SHA256=std::string(64,'f');});rejectCurrent([](auto&d){d.modifications[0].nativePhysicalEditKey="fl:native-channel:1";});rejectCurrent([](auto&d){d.regions[0].nativePhysicalRegionKey="fl:native-clip:999";});
        rejectCurrent([](auto&d){d.actualCopiedNativeModelRecords[40]^=1;},true);rejectCurrent([](auto&d){d.actualCopiedNativeModelRecords[3*88+32]^=1;},true);
        rejectCurrent([](auto&d){writeField(d.actualCopiedNativeModelRecords,2*88+32,std::int32_t(10));},true);
        rejectCurrent([](auto&d){std::swap_ranges(d.actualCopiedNativeModelRecords.begin(),d.actualCopiedNativeModelRecords.begin()+88,d.actualCopiedNativeModelRecords.begin()+88);},true);
        rejectCurrent([](auto&d){d.actualVerifiedAudioRecordIndices={0,1};});rejectCurrent([](auto&d){d.actualVerifiedAudioRecordIndices={0,1,2,3};});rejectCurrent([](auto&d){d.actualVerifiedAudioRecordIndices={0,1,1,2};});
        rejectCurrent([](auto&d){d.graph.regions[0].playbackStart=.1;},true);rejectCurrent([](auto&d){d.model.graphSHA256=std::string(64,'e');});rejectCurrent([](auto&d){d.model.nativeRecordModelSHA256=std::string(64,'e');});rejectCurrent([](auto&d){d.model.sourcePCMModelSHA256=std::string(64,'e');});
        rejectCurrent([&](auto&d){auto pcm=f.pcm;pcm[123]^=1;auto provider=std::make_shared<OfflinePCM>(d.graph.sources[0],pcm);d.resolveCurrentImmutableRAMSource=[provider](const SourceDescriptor&){return provider;};});
        rejectCurrent([&](auto&d){auto spec=d.graph.sources[0];spec.id="wrong";auto provider=std::make_shared<OfflinePCM>(spec,f.pcm);d.resolveCurrentImmutableRAMSource=[provider](const SourceDescriptor&){return provider;};});
        rejectCurrent([](auto&d){d.resolveCurrentImmutableRAMSource={};});
        auto rejectSaved=[&](const std::function<void(FLStandaloneStateEnvelope&)>& change){auto e=f.envelope;change(e);rejects([&]{encodeFLStandaloneStateEnvelope(e);});};
        rejectSaved([](auto&e){e.archive.schema=2;});rejectSaved([](auto&e){e.archive.ara.nativeGraphReady=false;});rejectSaved([](auto&e){e.archive.ara.factoryID="different";});
        rejectSaved([](auto&e){e.archive.nativeInsertKey="fl:native-mixer-insert:501";});rejectSaved([](auto&e){e.archive.native.clock.ppq=0;});rejectSaved([](auto&e){e.archive.native.clock.tempoMilliBPM=522001;});
        rejectSaved([](auto&e){e.archive.ara.pluginName=std::string(65537,'x');});rejectSaved([](auto&e){e.archive.ara.pluginName=std::string("x\0x",3);});rejectSaved([](auto&e){e.archive.ara.pluginName=std::string(1,char(255));});
        rejectSaved([](auto&e){e.archive.actualCopiedNativeModelRecords.push_back(0);});rejectSaved([](auto&e){e.archive.actualVerifiedAudioRecordIndices={0,2,1};});rejectSaved([](auto&e){e.archive.actualVerifiedAudioRecordIndices={0,1,999};});
        rejectSaved([](auto&e){e.archive.ara.graph.regions.push_back(e.archive.ara.graph.regions[0]);});rejectSaved([](auto&e){e.archive.ara.graph.sources[0].sampleCount=INT64_MAX;});rejectSaved([](auto&e){e.archive.ara.graph.sources[0].sampleRate=std::numeric_limits<double>::quiet_NaN();});
        for(Bytes plain:{Bytes{},Bytes{'a',0,255},Bytes{'R','Y','S','F','L','A','R','A'}}){auto copy=plain;check(!decodeFLStandaloneStateEnvelope(plain)&&plain==copy,"ordinary/old magic never mistaken for standalone envelope");}
        for(std::size_t size:{8u,12u,20u,51u})rejects([&]{decodeFLStandaloneStateEnvelope(Bytes(wire.begin(),wire.begin()+size));});
        for(unsigned kind=0;kind<4;++kind){auto b=wire;if(kind==0)b[8]=2;if(kind==1)b.back()^=1;if(kind==2)b[12]^=1;if(kind==3)b.push_back(0);rejects([&]{decodeFLStandaloneStateEnvelope(b);});}
        auto rejectWire=[&](const std::function<void(CFMutableDictionaryRef)>& change){auto b=mutatePlist(wire,change);rejects([&]{decodeFLStandaloneStateEnvelope(b);});};
        rejectWire([](auto d){CFDictionarySetValue(d,CFSTR("extra"),kCFBooleanTrue);});rejectWire([](auto d){CFDictionarySetValue(d,CFSTR("nativeComponentState"),CFSTR("not-data"));});
        rejectWire([](auto d){auto a=(CFMutableDictionaryRef)CFDictionaryGetValue(d,CFSTR("ara"));CFDictionarySetValue(a,CFSTR("nativeGraphReady"),kCFBooleanFalse);});
        rejectWire([](auto d){auto n=(CFMutableDictionaryRef)CFDictionaryGetValue(d,CFSTR("native"));setText(n,CFSTR("graphSHA256"),std::string(64,'a'));});
        rejectWire([](auto d){auto a=(CFMutableArrayRef)CFDictionaryGetValue(d,CFSTR("actualVerifiedAudioRecordIndices"));CFArrayAppendValue(a,CFArrayGetValueAtIndex(a,0));});
        Stream stream(new fl_app::MemoryIBStream(wire));check(fl_app::peekMagicAndRestorePosition(*stream),"standalone stream magic");int64 at=-1;stream->tell(&at);check(at==0,"peek restores exact position");check(fl_app::readRemaining(*stream)==wire,"read exact standalone wire");unsigned char scratch[8]{};int32 got=-1;check(stream->read(scratch,8,&got)==kResultFalse&&got==0,"EOF contract");stream->seek(-3,IBStream::kIBSeekEnd,nullptr);check(stream->read(scratch,8,&got)==kResultOk&&got==3,"partial read contract");check(stream->read(nullptr,0,&got)==kResultOk&&got==0,"zero read contract");
        void* object=nullptr;check(stream->queryInterface(INLINE_UID_OF(ISizeableStream),&object)==kResultOk,"sizeable stream QI");int64 size=0;auto* sizeable=static_cast<ISizeableStream*>(object);sizeable->getStreamSize(size);check(size==static_cast<int64>(wire.size()),"stream size");sizeable->release();
        Stream small(new fl_app::MemoryIBStream(Bytes{1,2,3}));rejects([&]{fl_app::readRemaining(*small,2);});Stream out(new fl_app::MemoryIBStream);fl_app::writeAll(*out,wire);check(out->bytes()==wire,"standalone write exact");
        std::cout<<"{\"status\":\"passed\",\"scope\":\"offline standalone codec/current immutable provider and synthetic raw88 relocation controls; dummy plugin/archive, not native FL or ARA proof\",\"checks\":"<<checks<<",\"encodedBytes\":"<<wire.size()<<"}\n";return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
#endif
