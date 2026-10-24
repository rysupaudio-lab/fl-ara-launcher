#include "FLStandaloneGraphDecoder.h"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace rysup::ara_lab {
namespace {
[[noreturn]] void fail(const char* message){throw FLStandaloneDecodeError(std::string("FLStandaloneGraphDecoder: ")+message);}
void demand(bool value,const char* message){if(!value)fail(message);}
bool validSHA(const std::string& s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string hash(const void* bytes,std::size_t count){
    demand(count<=std::numeric_limits<CC_LONG>::max(),"SHA input over budget");
    unsigned char result[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes,static_cast<CC_LONG>(count),result);
    static constexpr char h[]="0123456789abcdef";std::string out;out.reserve(64);
    for(auto v:result){out+=h[v>>4];out+=h[v&15];}return out;
}
std::string hash(const Bytes& value){return hash(value.data(),value.size());}
void add64(Bytes& out,std::uint64_t value){for(unsigned i=0;i<8;++i)out.push_back((value>>(8*i))&255);}
void addString(Bytes& out,const std::string& value){add64(out,value.size());out.insert(out.end(),value.begin(),value.end());}
void addDouble(Bytes& out,double value){demand(std::isfinite(value),"nonfinite model seal");std::uint64_t bits;std::memcpy(&bits,&value,8);add64(out,bits);}
template<class T>T unpack(const std::uint8_t* p,std::size_t at){T out{};std::memcpy(&out,p+at,sizeof out);return out;}
void validateAssociation(const FLStandaloneComponentAssociation& a){
    demand(a.actualComponent&&a.nativeOwnerObject&&a.nativePointerAssociationVerified&&!a.exactObjectAssociationEvidence.empty()
        &&a.mixerInsert>=0&&a.mixerInsert<=500,"actual native component/owner/mixer pointer association required");
}
void copyRAM(std::uintptr_t address,void* destination,std::size_t size){
    demand(address&&destination&&size<=std::numeric_limits<std::uintptr_t>::max()-address,"invalid direct native clock read range");
    mach_vm_size_t copied=0;auto result=mach_vm_read_overwrite(mach_task_self(),address,size,reinterpret_cast<mach_vm_address_t>(destination),&copied);
    demand(result==KERN_SUCCESS&&copied==size,"direct native clock memory unreadable");
}
std::uintptr_t address(std::uintptr_t base,std::uintptr_t offset){demand(base&&offset<=std::numeric_limits<std::uintptr_t>::max()-base,"native clock address overflow");return base+offset;}
FLStandaloneDirectClock readClock(const FLNativeEngineBinding& binding){
    FLStandaloneDirectClock out;out.engineImageBase=binding.imageBase;out.ppqAddress=address(binding.imageBase,kFLStandalonePPQOffset);
    out.tempoAddress=address(binding.imageBase,kFLStandaloneTempoMilliBPMOffset);
    copyRAM(out.ppqAddress,out.actualClockBytes.data(),2);copyRAM(out.tempoAddress,out.actualClockBytes.data()+2,4);
    out.ppq=unpack<std::uint16_t>(out.actualClockBytes.data(),0);out.tempoMilliBPM=unpack<std::uint32_t>(out.actualClockBytes.data(),2);
    demand(out.ppq>0&&out.tempoMilliBPM>0&&out.tempoMilliBPM<=1000000,"direct native PPQ/tempo is unsupported");
    out.bpm=double(out.tempoMilliBPM)/1000.0;out.engineArm64SliceSHA256=binding.verifiedArm64SliceSHA256;return out;
}
struct FD{int value=-1;~FD(){if(value>=0)close(value);}};
bool sameFile(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size
    &&a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec&&a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec
    &&a.st_ctimespec.tv_sec==b.st_ctimespec.tv_sec&&a.st_ctimespec.tv_nsec==b.st_ctimespec.tv_nsec;}
std::uint32_t be32(const Bytes& b,std::size_t off){demand(off<=b.size()&&b.size()-off>=4,"truncated engine file");return(std::uint32_t(b[off])<<24)|(std::uint32_t(b[off+1])<<16)|(std::uint32_t(b[off+2])<<8)|b[off+3];}
std::uint64_t be64(const Bytes& b,std::size_t off){return(std::uint64_t(be32(b,off))<<32)|be32(b,off+4);}
void verifyEngine(const FLNativeEngineBinding& binding,const FLStandaloneDecoderLimits& limits){
    demand(binding.verifiedArm64SliceSHA256==kFLNativeModelArm64SHA256&&!binding.exactLoadedImagePath.empty(),"unsupported engine slice binding");
    mach_header_64 loaded{};copyRAM(binding.imageBase,&loaded,sizeof loaded);
    Dl_info actual{};demand(dladdr(reinterpret_cast<const void*>(binding.imageBase),&actual)&&actual.dli_fbase==reinterpret_cast<void*>(binding.imageBase)
        &&actual.dli_fname&&binding.exactLoadedImagePath==actual.dli_fname&&loaded.magic==MH_MAGIC_64&&loaded.cputype==CPU_TYPE_ARM64,"actual loaded native engine header/path binding differs");
    FD fd{open(binding.exactLoadedImagePath.c_str(),O_RDONLY|O_CLOEXEC)};demand(fd.value>=0,"actual loaded engine file unreadable");struct stat before{},after{};
    demand(fstat(fd.value,&before)==0&&S_ISREG(before.st_mode)&&before.st_size>=32&&std::uint64_t(before.st_size)<=limits.maxEngineFileBytes,"actual engine file size/type over budget");
    // Cache a digest only for the same actual file inode/size/mtime/ctime in this
    // process. Changed metadata forces a fresh read; capture never trusts a path
    // string or the caller's digest alone. Native model capture also binds dladdr.
    struct SealedFile{struct stat stat{};std::string sha;};static std::map<std::string,SealedFile> sealed;
    auto found=sealed.find(binding.exactLoadedImagePath);
    if(found!=sealed.end()&&sameFile(found->second.stat,before)){demand(found->second.sha==binding.verifiedArm64SliceSHA256,"cached actual engine digest differs");return;}
    Bytes bytes(static_cast<std::size_t>(before.st_size));std::size_t read=0;
    while(read<bytes.size()){auto n=pread(fd.value,bytes.data()+read,bytes.size()-read,static_cast<off_t>(read));demand(n>0,"actual engine file read incomplete");read+=static_cast<std::size_t>(n);}
    demand(fstat(fd.value,&after)==0&&sameFile(before,after),"actual engine file changed while hashing");
    std::size_t start=0,count=bytes.size();const auto magic=be32(bytes,0);
    if(magic==0xcafebabeU||magic==0xcafebabfU){const bool wide=magic==0xcafebabfU;const auto architectures=be32(bytes,4);const std::size_t width=wide?32:20;
        demand(architectures>0&&architectures<=32&&8+std::uint64_t(architectures)*width<=bytes.size(),"invalid universal engine architecture table");unsigned matches=0;
        for(std::size_t i=0;i<architectures;++i){const auto at=8+i*width;if(be32(bytes,at)!=std::uint32_t(CPU_TYPE_ARM64))continue;
            const auto off=wide?be64(bytes,at+8):be32(bytes,at+8);const auto length=wide?be64(bytes,at+16):be32(bytes,at+12);
            demand(off<=bytes.size()&&length<=bytes.size()-off&&length>=sizeof(mach_header_64),"engine ARM64 slice extent invalid");start=static_cast<std::size_t>(off);count=static_cast<std::size_t>(length);++matches;}
        demand(matches==1,"unique actual ARM64 engine slice missing");
    }
    demand(unpack<std::uint32_t>(bytes.data()+start,0)==MH_MAGIC_64&&unpack<std::uint32_t>(bytes.data()+start,4)==std::uint32_t(CPU_TYPE_ARM64),"engine slice header is not ARM64");
    auto sha=hash(bytes.data()+start,count);demand(sha==binding.verifiedArm64SliceSHA256,"actual engine ARM64 slice digest differs");sealed[binding.exactLoadedImagePath]={before,sha};
}
class PCMReader final:public SourceReader{
public:PCMReader(std::shared_ptr<const Bytes>b,SourceDescriptor d,bool wide):bytes(std::move(b)),spec(std::move(d)),doublePrecision(wide){}
    bool read(std::int64_t start,std::int64_t count,void* const output[])noexcept override{
        if(count<0||!output||std::uint64_t(count)>std::numeric_limits<std::size_t>::max()/(doublePrecision?sizeof(double):sizeof(float)))return false;
        for(int c=0;c<spec.channelCount;++c)if(!output[c])return false;
        for(int c=0;c<spec.channelCount;++c){if(doublePrecision)std::fill_n(static_cast<double*>(output[c]),static_cast<std::size_t>(count),0.0);else std::fill_n(static_cast<float*>(output[c]),static_cast<std::size_t>(count),0.0f);}
        for(std::int64_t out=0;out<count;++out){if(start>std::numeric_limits<std::int64_t>::max()-out)break;const auto frame=start+out;if(frame<0||frame>=spec.sampleCount)continue;
            for(int c=0;c<spec.channelCount;++c){const auto off=(std::size_t(frame)*std::size_t(spec.channelCount)+std::size_t(c))*2;
                const auto u=std::uint16_t((*bytes)[off])|(std::uint16_t((*bytes)[off+1])<<8);std::int16_t sample;std::memcpy(&sample,&u,2);
                if(doublePrecision)static_cast<double*>(output[c])[out]=double(sample)/32768.0;else static_cast<float*>(output[c])[out]=float(sample)/32768.0f;}}
        return true;
    }
private:std::shared_ptr<const Bytes>bytes;SourceDescriptor spec;bool doublePrecision;
};
class PCMProvider final:public SourceProvider{
public:PCMProvider(SourceDescriptor d,const Bytes& b):spec(std::move(d)),bytes(std::make_shared<const Bytes>(b)){}
    SourceDescriptor describe()const override{return spec;}
    std::unique_ptr<SourceReader>openReader(bool wide)override{return std::make_unique<PCMReader>(bytes,spec,wide);}
private:SourceDescriptor spec;std::shared_ptr<const Bytes>bytes;
};
std::string graphHash(const Graph& graph){
    auto g=graph;auto order=[](const auto&a,const auto&b){return a.id<b.id;};std::sort(g.sources.begin(),g.sources.end(),order);std::sort(g.tracks.begin(),g.tracks.end(),order);
    std::sort(g.modifications.begin(),g.modifications.end(),order);std::sort(g.regions.begin(),g.regions.end(),order);Bytes out;
    addString(out,"rysup.private-fl.exact-editor-graph/1");addString(out,g.documentName);
    add64(out,g.sources.size());for(const auto&s:g.sources){addString(out,s.id);addString(out,s.name);add64(out,s.sampleCount);addDouble(out,s.sampleRate);add64(out,s.channelCount);}
    add64(out,g.tracks.size());for(const auto&t:g.tracks){addString(out,t.id);addString(out,t.name);}
    add64(out,g.modifications.size());for(const auto&m:g.modifications){addString(out,m.id);addString(out,m.sourceID);addString(out,m.name);}
    add64(out,g.regions.size());for(const auto&r:g.regions){for(const auto*s:{&r.id,&r.modificationID,&r.trackID,&r.name})addString(out,*s);
        for(auto x:{r.sourceStart,r.sourceDuration,r.playbackStart,r.playbackDuration})addDouble(out,x);add64(out,r.transformations);}return hash(out);
}
FLStandaloneDecodedGraph buildGraph(const FLNativeModelSnapshot& model,const FLStandaloneDirectClock& clock,
    const FLStandaloneComponentAssociation& association,const std::string& decoderSHA,const FLStandaloneDecoderLimits& limits,bool actualCapture){
    validateAssociation(association);demand(validSHA(decoderSHA),"compiled decoder source SHA missing");
    demand(model.repeatedReadsMatched&&model.nativeRecordStride==88&&model.nativeRecordCount>=0&&std::size_t(model.nativeRecordCount)==model.clips.size()
        &&model.clips.size()<=limits.maxNativeClips,"native RAM model lacks exact repeated-read/record extent proof");
    demand(model.verifiedArm64SliceSHA256==kFLNativeModelArm64SHA256&&clock.engineArm64SliceSHA256==model.verifiedArm64SliceSHA256
        &&clock.engineImageBase==model.engineImageBase&&clock.ppqAddress==address(model.engineImageBase,kFLStandalonePPQOffset)
        &&clock.tempoAddress==address(model.engineImageBase,kFLStandaloneTempoMilliBPMOffset)&&clock.actualDirectNativeReadTwiceAndEqual,"native RAM/direct clock engine identity differs");
    demand(clock.ppq>0&&clock.tempoMilliBPM>0&&clock.tempoMilliBPM<=1000000&&clock.bpm==double(clock.tempoMilliBPM)/1000.0
        &&clock.ppq==unpack<std::uint16_t>(clock.actualClockBytes.data(),0)&&clock.tempoMilliBPM==unpack<std::uint32_t>(clock.actualClockBytes.data(),2),"direct clock typed values/actual bytes differ");
    demand(model.currentArrangementIndex>=0&&std::size_t(model.currentArrangementIndex)<model.arrangementObjects.size()
        &&model.currentArrangementObject&&model.arrangementObjects[model.currentArrangementIndex]==model.currentArrangementObject,"current native arrangement binding differs");
    demand(model.nativeChannelCount>=0&&model.nativeChannelCount<=65536&&model.channels.size()<=65536,"native channel manager extent unsupported");
    std::map<std::uint16_t,std::size_t> channelIndices;for(std::size_t i=0;i<model.channels.size();++i)demand(model.channels[i].managerIndex<model.nativeChannelCount&&channelIndices.emplace(model.channels[i].managerIndex,i).second,"duplicate/out-of-range native channel manager ID");
    demand(model.allNativeManagerChannelsInspected&&model.nativeManagerMembers.size()==std::size_t(model.nativeChannelCount),"complete actual native manager classification is required");
    for(std::size_t i=0;i<model.nativeManagerMembers.size();++i){const auto&member=model.nativeManagerMembers[i];
        demand(member.managerIndex==i&&member.actualChannelObject&&member.actualChannelVMT,"actual native manager member index/object/class proof invalid");
        const auto found=channelIndices.find(member.managerIndex);
        if(member.nativeChannelType150==4)demand(found!=channelIndices.end(),"actual unreferenced native audio channel missing from copied model");
        if(found!=channelIndices.end()){const auto&channel=model.channels[found->second];demand(channel.channelObject==member.actualChannelObject&&channel.actualChannelVMT==member.actualChannelVMT
            &&channel.nativeChannelType150==member.nativeChannelType150,"copied native channel differs from complete actual manager classification");}
    }
    demand(model.pcmCopyFilterMixerInsert==association.mixerInsert,"native PCM copy filter does not match actual selected insert");
    std::set<std::size_t>selectedNativeSources;
    for(const auto&channel:model.channels)if(channel.nativeChannelType150==4&&channel.nativeMixerInsert248==association.mixerInsert)
        selectedNativeSources.insert(channel.sourceSnapshotIndex);
    std::size_t actualCopiedBytes=0;
    for(std::size_t i=0;i<model.sources.size();++i){const auto&source=model.sources[i];
        demand(source.pcmBytes.empty()||selectedNativeSources.count(i),"unrelated native source PCM was copied despite selected-insert filter");
        demand(source.pcmBytes.size()<=limits.maxPCMBytesTotal&&actualCopiedBytes<=limits.maxPCMBytesTotal-source.pcmBytes.size(),"actual native PCM copy total exceeds budget");
        actualCopiedBytes+=source.pcmBytes.size();
    }
    demand(actualCopiedBytes==model.pcmBytesCopied,"actual copied PCM byte accounting differs from native reader report");
    FLStandaloneDecodedGraph out;out.graph.documentName="FL ARA";out.clock=clock;out.actualAssociation=association;out.nativeArrangementIndex=model.currentArrangementIndex;
    out.actualNativeManagerMembers=model.nativeManagerMembers;out.actualNativeManagerChannelCount=model.nativeChannelCount;out.allNativeManagerChannelsInspected=model.allNativeManagerChannelsInspected;
    out.actualPCMCopyMixerInsertFilter=model.pcmCopyFilterMixerInsert;out.actualOwnedPCMBytesCopied=model.pcmBytesCopied;
    std::map<std::string,std::shared_ptr<SourceProvider>> providers;std::map<std::size_t,std::string>nativePCMHashes;std::set<std::string>tracks;std::set<std::int32_t>clipIDs;std::size_t totalPCM=0;Bytes normalized;std::vector<std::array<std::uint8_t,88>> records;
    std::map<std::uint16_t,std::string>selectedSourceIDs;
    for(const auto&channel:model.channels){
        if(channel.nativeChannelType150!=4)continue;
        demand(channel.channelObject&&channel.nativeMixerInsertSupported&&channel.nativeMixerInsert248>=0&&channel.nativeMixerInsert248<=500,"actual native audio channel mixer assignment unsupported");
        if(channel.nativeMixerInsert248!=association.mixerInsert)continue;
        demand(channel.sourceSnapshotIndex<model.sources.size(),"selected native audio channel has no copied source");const auto&source=model.sources[channel.sourceSnapshotIndex];
        demand(source.waveSampleObject&&source.actualWaveSampleVMT&&channel.nativeWaveSampleObject328==source.waveSampleObject&&channel.actualNativeWaveSampleVMT==source.actualWaveSampleVMT,"actual native channel +328 wave source identity differs");
        demand(source.qualifiedEncoding==FLNativePCMEncoding::SignedInt16LE&&(source.qualifiedChannelCount==1||source.qualifiedChannelCount==2)
            &&source.nativeFormatByte78==source.qualifiedChannelCount&&source.nativeFormatByte79==0&&!source.nativeReferenceC8&&!source.pcmQualificationEvidenceID.empty(),"source PCM format/residency not explicitly qualified");
        demand(source.nativeRate88==48000.0&&source.nativeRateFactor1A0==1.0&&source.nativeFrameCount98>0&&std::uint64_t(source.nativeFrameCount98)<=std::numeric_limits<std::size_t>::max()/(source.qualifiedChannelCount*2),"unsupported source rate/factor/frame extent");
        const auto size=std::size_t(source.nativeFrameCount98)*source.qualifiedChannelCount*2;demand(size<=limits.maxPCMBytesPerSource&&source.pcmBytes.size()==size&&source.nativePCMAddress90,"native PCM copy absent/truncated/over budget");
        auto cachedHash=nativePCMHashes.find(channel.sourceSnapshotIndex);if(cachedHash==nativePCMHashes.end())cachedHash=nativePCMHashes.emplace(channel.sourceSnapshotIndex,hash(source.pcmBytes)).first;
        const auto&pcmSHA=cachedHash->second;const auto channelKey="fl:native-channel:"+std::to_string(channel.managerIndex);const auto sourceID=channelKey+":pcm:"+pcmSHA;
        const auto modificationID=sourceID+":modification";
        if(!providers.count(sourceID)){
            demand(totalPCM<=limits.maxPCMBytesTotal&&size<=limits.maxPCMBytesTotal-totalPCM,"native PCM total budget exceeded");totalPCM+=size;
            SourceDescriptor descriptor{sourceID,"",source.nativeFrameCount98,source.nativeRate88,int(source.qualifiedChannelCount)};providers.emplace(sourceID,std::make_shared<PCMProvider>(descriptor,source.pcmBytes));
            out.graph.sources.push_back(descriptor);out.graph.modifications.push_back({modificationID,sourceID,""});
            out.sources.push_back({sourceID,channelKey,pcmSHA,channel.managerIndex,channel.nativeMixerInsert248,source.nativeFrameCount98,source.qualifiedChannelCount,
                source.nativeRate88,source.nativeRateFactor1A0,source.nativeFormatByte78,source.nativeFormatByte79,source.pcmQualificationEvidenceID});
            out.modifications.push_back({modificationID,channelKey,sourceID});
        }
        demand(selectedSourceIDs.emplace(channel.managerIndex,sourceID).second,"duplicate selected native audio channel identity");
    }
    for(std::size_t index=0;index<model.clips.size();++index){const auto&clip=model.clips[index];const auto*p=clip.record.data();
        demand(clip.nativeListIndex==index&&clip.nativeClipID20==unpack<std::int32_t>(p,32)&&clip.packedTypeAndChannel04==unpack<std::uint32_t>(p,4)
            &&clip.channelIndex06==unpack<std::uint16_t>(p,6)&&clip.timelineStartTicks00==unpack<std::int32_t>(p,0)&&clip.timelineLengthTicks08==unpack<std::int32_t>(p,8)
            &&clip.nativeTrack0C==unpack<std::int16_t>(p,12)&&std::memcmp(p+24,&clip.nativeSourceStart18,4)==0&&std::memcmp(p+28,&clip.nativeSourceEnd1C,4)==0,"native clip typed fields differ from actual raw88");
        out.actualCopiedNativeModelRecords.insert(out.actualCopiedNativeModelRecords.end(),clip.record.begin(),clip.record.end());records.push_back(clip.record);
        auto canonical=clip.record;std::fill(canonical.begin()+32,canonical.begin()+36,0);normalized.insert(normalized.end(),canonical.begin(),canonical.end());
        if(clip.kind==FLNativeRecordKind::AudioChannel)out.actualVerifiedAudioRecordIndices.push_back(index);
        demand(clip.kind!=FLNativeRecordKind::Unresolved,"unresolved native Playlist record cannot be silently omitted");if(clip.kind!=FLNativeRecordKind::AudioChannel)continue;
        demand(clip.nativeClipID20>0&&clipIDs.insert(clip.nativeClipID20).second&&clip.packedTypeAndChannel04!=0xffffffffU&&std::int32_t(clip.packedTypeAndChannel04)<0x50000000,"native audio record kind/clip ID invalid or duplicated");
        demand(clip.channelSnapshotIndex<model.channels.size(),"actual audio clip channel absent");const auto&channel=model.channels[clip.channelSnapshotIndex];
        demand(channel.managerIndex==clip.channelIndex06&&channel.nativeChannelType150==4&&channel.channelObject&&channel.nativeMixerInsertSupported
            &&channel.nativeMixerInsert248>=0&&channel.nativeMixerInsert248<=500,"actual audio channel/type/mixer identity unsupported");
        if(channel.nativeMixerInsert248!=association.mixerInsert)continue;
        const auto selectedSource=selectedSourceIDs.find(channel.managerIndex);demand(selectedSource!=selectedSourceIDs.end(),"selected actual audio channel has no admitted copied PCM identity");
        const auto&source=model.sources[channel.sourceSnapshotIndex];const auto&sourceID=selectedSource->second;const auto modificationID=sourceID+":modification";
        demand(clip.timelineStartTicks00>=0&&clip.timelineLengthTicks08>0&&clip.nativeTrack0C>=0&&std::isfinite(clip.nativeSourceStart18)
            &&std::isfinite(clip.nativeSourceEnd1C)&&clip.nativeSourceStart18>=0&&clip.nativeSourceEnd1C>clip.nativeSourceStart18,"invalid timeline/crop/track geometry");
        const double startFrames=double(clip.nativeSourceStart18)*source.nativeRate88*source.nativeRateFactor1A0*.001;
        const double endFrames=double(clip.nativeSourceEnd1C)*source.nativeRate88*source.nativeRateFactor1A0*.001;
        const double start=startFrames/source.nativeRate88,duration=(endFrames-startFrames)/source.nativeRate88;
        const double playbackStart=double(clip.timelineStartTicks00)/clock.ppq*60.0/clock.bpm,playbackDuration=double(clip.timelineLengthTicks08)/clock.ppq*60.0/clock.bpm;
        demand(std::isfinite(startFrames)&&std::isfinite(endFrames)&&endFrames<=double(source.nativeFrameCount98)+1e-7&&std::isfinite(playbackStart)
            &&std::isfinite(playbackDuration)&&std::abs(duration-playbackDuration)<=1e-8,"crop beyond PCM or native stretch unsupported");
        const auto nativeClip="fl:native-clip:"+std::to_string(clip.nativeClipID20),regionID=nativeClip+":"+sourceID+":region",trackID="fl:native-playlist-track:"+std::to_string(clip.nativeTrack0C);
        if(tracks.insert(trackID).second)out.graph.tracks.push_back({trackID,""});
        out.graph.regions.push_back({regionID,modificationID,trackID,"",start,duration,playbackStart,playbackDuration,ARA::kARAPlaybackTransformationNoChanges});
        out.regions.push_back({regionID,nativeClip,modificationID,sourceID,clip.nativeClipID20,channel.managerIndex,index,clip.record,hash(p,88),hash(canonical.data(),88)});
    }
    // Zero regions retains real channel/source/modification objects for Undo.
    // A genuinely empty selected insert is represented by a real empty Graph;
    // the complete native manager inventory above is the authority for absence.
    auto order=[](const auto&a,const auto&b){return a.id<b.id;};std::sort(out.graph.sources.begin(),out.graph.sources.end(),order);std::sort(out.graph.tracks.begin(),out.graph.tracks.end(),order);
    std::sort(out.graph.modifications.begin(),out.graph.modifications.end(),order);std::sort(out.graph.regions.begin(),out.graph.regions.end(),order);
    out.resolveCurrentImmutableRAMSource=[providers=std::move(providers)](const SourceDescriptor& wanted){auto it=providers.find(wanted.id);demand(it!=providers.end(),"source not in captured immutable PCM");const auto have=it->second->describe();
        demand(wanted.name==have.name&&wanted.sampleCount==have.sampleCount&&wanted.sampleRate==have.sampleRate&&wanted.channelCount==have.channelCount,"source descriptor differs from captured PCM");return it->second;};
    Bytes rawModel;addString(rawModel,"rysup.private-fl.native88-record-multiset/1");add64(rawModel,records.size());std::sort(records.begin(),records.end());for(const auto&r:records)rawModel.insert(rawModel.end(),r.begin(),r.end());
    out.nativeRecordModelSHA256=hash(rawModel);out.orderedNativeRecordsSHA256=hash(out.actualCopiedNativeModelRecords);out.normalizedOrderedNativeRecordsSHA256=hash(normalized);
    Bytes sourceModel;addString(sourceModel,"rysup.standalone-fl.native-channel-copied-pcm/1");add64(sourceModel,association.mixerInsert);
    auto sources=out.sources;std::sort(sources.begin(),sources.end(),[](const auto&a,const auto&b){return a.sourceID<b.sourceID;});add64(sourceModel,sources.size());
    for(const auto&s:sources){addString(sourceModel,s.sourceID);add64(sourceModel,s.nativeChannelIndex);add64(sourceModel,s.nativeMixerInsert);add64(sourceModel,s.sampleCount);add64(sourceModel,s.channelCount);
        addDouble(sourceModel,s.sampleRate);addDouble(sourceModel,s.rateFactor);add64(sourceModel,s.formatByte78);add64(sourceModel,s.formatByte79);addString(sourceModel,s.actualPCM_SHA256);}
    out.sourcePCMModelSHA256=hash(sourceModel);out.graphSHA256=graphHash(out.graph);out.decoderIdentity="rysup.standalone-fl.actual-native-ram-direct-clock/1";
    out.decoderSourceSHA256=decoderSHA;out.actualCurrentNativeRAMCopiedAndVerified=actualCapture;out.noNativeSerializationOrFileAudioFallbackUsed=true;return out;
}
} // namespace
struct CapturedFLStandaloneModel::Impl{std::shared_ptr<const FLNativeModelSnapshot>model;FLStandaloneDirectClock clock;FLStandaloneComponentAssociation association;};
CapturedFLStandaloneModel::CapturedFLStandaloneModel(std::shared_ptr<const Impl>value):p(std::move(value)){}
const FLNativeModelSnapshot&CapturedFLStandaloneModel::model()const noexcept{return*p->model;}
const FLStandaloneDirectClock&CapturedFLStandaloneModel::clock()const noexcept{return p->clock;}
const FLStandaloneComponentAssociation&CapturedFLStandaloneModel::association()const noexcept{return p->association;}
struct VerifiedFLStandaloneEngine::Impl{FLNativeEngineBinding binding;mach_header_64 actualLoadedHeader{};};
VerifiedFLStandaloneEngine::VerifiedFLStandaloneEngine(std::shared_ptr<const Impl>value):p(std::move(value)){}
const FLNativeEngineBinding&VerifiedFLStandaloneEngine::binding()const noexcept{return p->binding;}
VerifiedFLStandaloneEngine FLStandaloneGraphDecoder::verifyActualLoadedEngine(const FLNativeEngineBinding&binding,const FLStandaloneDecoderLimits&limits){
    demand(pthread_main_np()!=0,"actual native engine verification requires main/model thread");verifyEngine(binding,limits);
    auto impl=std::make_shared<VerifiedFLStandaloneEngine::Impl>();impl->binding=binding;copyRAM(binding.imageBase,&impl->actualLoadedHeader,sizeof impl->actualLoadedHeader);
    return VerifiedFLStandaloneEngine(std::move(impl));
}
CapturedFLStandaloneModel FLStandaloneGraphDecoder::captureActualCurrentRAMAtNativeLock(const VerifiedFLStandaloneEngine&seal,const FLNativeModelOptions&options,
    const FLStandaloneComponentAssociation&association,const FLStandaloneDecoderLimits&limits){
    demand(pthread_main_np()!=0,"actual native capture requires main/model thread");validateAssociation(association);
    demand(options.captureAllAudioChannels&&options.copyQualifiedPCM&&options.requireSupportedMixerInsertForEveryAudioChannel,"capture must inspect all native audio channels, copy qualified PCM and require supported mixer assignment");
    demand(options.copyQualifiedPCMOnlyForMixerInsert==association.mixerInsert,"PCM copy filter must match actual selected native mixer insert");
    // Unrelated unqualified audio sources may remain descriptors. Every selected
    // source is independently required qualified/copied by buildGraph().
    const auto&binding=seal.binding();mach_header_64 current{};copyRAM(binding.imageBase,&current,sizeof current);
    demand(std::memcmp(&current,&seal.p->actualLoadedHeader,sizeof current)==0,"actual engine header changed after outside-lock verification");
    demand(options.limits.maxClips<=limits.maxNativeClips&&options.limits.maxPCMBytesPerSource<=limits.maxPCMBytesPerSource
        &&options.limits.maxPCMBytesTotal<=limits.maxPCMBytesTotal,"native capture options exceed decoder budgets");
    auto before=readClock(binding);auto model=FLNativeModel::capture(binding,options);auto after=readClock(binding);
    demand(before.actualClockBytes==after.actualClockBytes,"direct native PPQ/tempo changed across immutable native model copy");
    demand(model&&model->engineImageBase==binding.imageBase&&model->verifiedArm64SliceSHA256==binding.verifiedArm64SliceSHA256&&model->loadedImagePath==binding.exactLoadedImagePath,"captured native model differs from direct clock engine binding");
    after.actualDirectNativeReadTwiceAndEqual=true;auto impl=std::make_shared<CapturedFLStandaloneModel::Impl>();impl->model=std::move(model);impl->clock=std::move(after);impl->association=association;
    return CapturedFLStandaloneModel(std::move(impl));
}
CapturedFLStandaloneModel FLStandaloneGraphDecoder::captureActualCurrentRAM(const FLNativeEngineBinding&binding,const FLNativeModelOptions&options,
    const FLStandaloneComponentAssociation&association,const FLStandaloneDecoderLimits&limits){
    auto seal=verifyActualLoadedEngine(binding,limits);return captureActualCurrentRAMAtNativeLock(seal,options,association,limits);
}
FLStandaloneDecodedGraph FLStandaloneGraphDecoder::decode(const CapturedFLStandaloneModel&capture,const std::string&sourceSHA,const FLStandaloneDecoderLimits&limits){
    demand(pthread_main_np()!=0,"actual native decode requires main/model thread");return buildGraph(capture.model(),capture.clock(),capture.association(),sourceSHA,limits,true);
}
} // namespace rysup::ara_lab

#ifdef FL_STANDALONE_GRAPH_DECODER_OFFLINE_TEST_MAIN
// Explicitly synthetic, pure graph/PCM controls. Never calls capture(), reads a
// loaded FL process or constructs the private actual-native capture token.
#include <iostream>
#include <type_traits>
namespace rysup::ara_lab {
namespace {
template<class T>void put(std::array<std::uint8_t,88>&raw,std::size_t at,T value){std::memcpy(raw.data()+at,&value,sizeof value);}
void updateRaw(FLNativeClipSnapshot&clip){
    put(clip.record,0,clip.timelineStartTicks00);put(clip.record,4,clip.packedTypeAndChannel04);put(clip.record,8,clip.timelineLengthTicks08);
    put(clip.record,12,clip.nativeTrack0C);put(clip.record,24,clip.nativeSourceStart18);put(clip.record,28,clip.nativeSourceEnd1C);put(clip.record,32,clip.nativeClipID20);
}
FLNativeModelSnapshot syntheticModel(){
    FLNativeModelSnapshot m;m.engineImageBase=0x100000000;m.verifiedArm64SliceSHA256=kFLNativeModelArm64SHA256;m.repeatedReadsMatched=true;m.nativeRecordStride=88;
    m.nativeChannelCount=2;m.currentArrangementIndex=0;m.currentArrangementObject=0x200000;m.arrangementObjects={m.currentArrangementObject};
    for(int c=0;c<2;++c){FLNativeSourceSnapshot s;s.waveSampleObject=0x300000+c*0x1000;s.actualWaveSampleVMT=0x400000;s.nativeFrameCount98=96000;s.nativeRate88=48000;
        s.nativeRateFactor1A0=1;s.nativeFormatByte78=c+1;s.nativeFormatByte79=0;s.nativePCMAddress90=0x500000+c*0x1000;s.qualifiedEncoding=FLNativePCMEncoding::SignedInt16LE;
        s.qualifiedChannelCount=c+1;s.pcmQualificationEvidenceID="SYNTHETIC offline control, not actual native evidence";
        for(int f=0;f<96000;++f)for(int ch=0;ch<=c;++ch){std::int16_t sample=static_cast<std::int16_t>(((f*13+ch*17)%65536)-32768);s.pcmBytes.push_back(sample&255);s.pcmBytes.push_back((std::uint16_t(sample)>>8)&255);}
        m.sources.push_back(s);FLNativeChannelSnapshot channel;channel.managerIndex=c;channel.channelObject=0x600000+c*0x1000;channel.actualChannelVMT=0x650000;channel.nativeChannelType150=4;
        channel.nativeMixerInsert248=1;channel.nativeMixerInsertSupported=true;channel.sourceSnapshotIndex=c;channel.nativeWaveSampleObject328=s.waveSampleObject;channel.actualNativeWaveSampleVMT=s.actualWaveSampleVMT;m.channels.push_back(channel);
        for(int r=0;r<3;++r){FLNativeClipSnapshot clip;clip.nativeListIndex=m.clips.size();clip.nativeClipID20=int(m.clips.size())+1;clip.channelIndex06=c;
            clip.packedTypeAndChannel04=0x5000|(std::uint32_t(c)<<16);clip.kind=FLNativeRecordKind::AudioChannel;clip.channelSnapshotIndex=c;clip.nativeTrack0C=c;
            clip.timelineStartTicks00=(r==0?0:(r==1?3:5))*1920;clip.timelineLengthTicks08=r==0?3840:(r==1?1920:1440);
            clip.nativeSourceStart18=r==0?0.0f:(r==1?250.0f:500.0f);clip.nativeSourceEnd1C=clip.nativeSourceStart18+(r==0?2000.0f:(r==1?1000.0f:750.0f));updateRaw(clip);m.clips.push_back(clip);}}
    m.pcmCopyFilterMixerInsert=1;
    for(const auto&source:m.sources)m.pcmBytesCopied+=source.pcmBytes.size();
    m.allNativeManagerChannelsInspected=true;
    for(const auto&channel:m.channels)m.nativeManagerMembers.push_back({channel.managerIndex,channel.channelObject,channel.actualChannelVMT,channel.nativeChannelType150});
    m.nativeRecordCount=int(m.clips.size());return m;
}
FLStandaloneDirectClock syntheticClock(const FLNativeModelSnapshot&m){FLStandaloneDirectClock c;c.engineImageBase=m.engineImageBase;c.engineArm64SliceSHA256=kFLNativeModelArm64SHA256;
    c.ppq=960;c.tempoMilliBPM=120000;c.bpm=120;c.ppqAddress=m.engineImageBase+kFLStandalonePPQOffset;c.tempoAddress=m.engineImageBase+kFLStandaloneTempoMilliBPMOffset;
    std::memcpy(c.actualClockBytes.data(),&c.ppq,2);std::memcpy(c.actualClockBytes.data()+2,&c.tempoMilliBPM,4);c.actualDirectNativeReadTwiceAndEqual=true;return c;}
int offlineMain(){
    static_assert(!std::is_default_constructible<CapturedFLStandaloneModel>::value,"actual native capture cannot be default-fabricated");
    static_assert(!std::is_constructible<CapturedFLStandaloneModel,std::shared_ptr<const FLNativeModelSnapshot>>::value,"actual token cannot accept a fabricated model");
    auto m=syntheticModel();auto clock=syntheticClock(m);FLStandaloneComponentAssociation a{0x700000,0x800000,1,"SYNTHETIC actual-pointer contract control only",true};
    const std::string sha(64,'a');std::vector<std::string>passed;
    auto pass=[&](const std::string&text){passed.push_back(text);};
    auto make=[&](const FLNativeModelSnapshot&model){return buildGraph(model,clock,a,sha,{},false);};
    auto reject=[&](const char*label,auto test){bool failed=false;try{test();}catch(const FLStandaloneDecodeError&){failed=true;}demand(failed,label);pass(label);};
    static_assert(!std::is_default_constructible<VerifiedFLStandaloneEngine>::value,"actual engine seal cannot be default-fabricated");
    auto base=make(m);demand(base.graph.sources.size()==2&&base.graph.modifications.size()==2&&base.graph.regions.size()==6,"shared native channel does not own exactly one modification");
    demand(base.actualVerifiedAudioRecordIndices==std::vector<std::size_t>({0,1,2,3,4,5}),"actual model audio row classification not retained");
    demand(!base.actualCurrentNativeRAMCopiedAndVerified&&base.noNativeSerializationOrFileAudioFallbackUsed,"synthetic test falsely claims actual capture");
    for(const auto&r:base.graph.regions)demand(r.modificationID==r.id.substr(r.id.find("fl:native-channel:")).substr(0,r.id.substr(r.id.find("fl:native-channel:")).size()-7)+":modification","region did not reference shared native-channel modification");
    pass("two native channels, six regions, exactly two shared modifications; synthetic capture remains unqualified");
    for(const auto&s:base.graph.sources){auto provider=base.resolveCurrentImmutableRAMSource(s);const int channel=int(s.id[s.id.find("channel:")+8]-'0');const auto&bytes=m.sources[channel].pcmBytes;
        for(bool wide:{false,true}){std::vector<std::vector<float>>f(s.channelCount,std::vector<float>(s.sampleCount));std::vector<std::vector<double>>d(s.channelCount,std::vector<double>(s.sampleCount));std::vector<void*>out;
            for(int c=0;c<s.channelCount;++c)out.push_back(wide?static_cast<void*>(d[c].data()):static_cast<void*>(f[c].data()));auto reader=provider->openReader(wide);demand(reader->read(0,s.sampleCount,out.data()),"full source read failed");
            for(std::int64_t frame=0;frame<s.sampleCount;++frame)for(int c=0;c<s.channelCount;++c){auto sample=unpack<std::int16_t>(bytes.data(),(frame*s.channelCount+c)*2);demand((wide?d[c][frame]:double(f[c][frame]))==double(sample)/32768.0,"PCM provider changed ordered source value");}
            demand(reader->read(-2,8,out.data()),"negative source read failed");for(int c=0;c<s.channelCount;++c)for(int i=0;i<2;++i)demand((wide?d[c][i]:f[c][i])==0,"negative source extent not zero-filled");
            demand(reader->read(s.sampleCount-2,8,out.data()),"tail source read failed");for(int c=0;c<s.channelCount;++c)for(int i=2;i<8;++i)demand((wide?d[c][i]:f[c][i])==0,"tail source extent not zero-filled");
            demand(reader->read(std::numeric_limits<std::int64_t>::max(),8,out.data()),"overflow zero-fill failed");for(int c=0;c<s.channelCount;++c)for(int i=0;i<8;++i)demand((wide?d[c][i]:f[c][i])==0,"overflow source extent leaked PCM");
            demand(!reader->read(0,-1,out.data())&&!reader->read(0,1,nullptr),"invalid source outputs/range accepted");}
        pass(s.channelCount==1?"mono PCM16 float32/64 exact full extent and zero-fill/overflow boundaries":"stereo PCM16 ordered float32/64 exact full extent and zero-fill/overflow boundaries");}
    {auto altered=m;std::fill(altered.sources[0].pcmBytes.begin(),altered.sources[0].pcmBytes.end(),0);const auto&spec=base.graph.sources[0];auto reader=base.resolveCurrentImmutableRAMSource(spec)->openReader(false);float sample=0;void*out[]={&sample};demand(reader->read(0,1,out)&&sample==-1.0f,"copied provider backing changed with native model input");pass("provider owns immutable copied PCM backing");}
    {auto unrelated=m;unrelated.channels[1].nativeMixerInsert248=2;unrelated.pcmBytesCopied-=unrelated.sources[1].pcmBytes.size();unrelated.sources[1].pcmBytes.clear();auto result=make(unrelated);
        demand(result.graph.sources.size()==1&&result.graph.regions.size()==3&&result.actualVerifiedAudioRecordIndices==std::vector<std::size_t>({0,1,2,3,4,5}),"audio row classification was filtered by selected insert");
        pass("verified audio row classification includes ALL inserts before selected graph filtering");}
    {auto changed=base.graph.sources[0];changed.sampleCount--;reject("resolver refuses a source descriptor not matching immutable captured backing",[&]{base.resolveCurrentImmutableRAMSource(changed);});}
    auto mods=[](const FLStandaloneDecodedGraph&g){std::vector<std::pair<std::string,std::string>>out;for(const auto&x:g.graph.modifications)out.emplace_back(x.id,x.sourceID);return out;};
    {auto moved=m;moved.clips[0].timelineStartTicks00+=1920;moved.clips[0].nativeTrack0C+=2;updateRaw(moved.clips[0]);auto result=make(moved);demand(mods(result)==mods(base)&&result.graph.regions.size()==6&&result.graphSHA256!=base.graphSHA256&&result.sourcePCMModelSHA256==base.sourcePCMModelSHA256,"move changed source/edit identity");pass("native region move/track change preserves source/modification identity");}
    {auto trim=m;trim.clips[0].nativeSourceStart18=250;trim.clips[0].nativeSourceEnd1C=1250;trim.clips[0].timelineLengthTicks08=1920;updateRaw(trim.clips[0]);auto result=make(trim);demand(mods(result)==mods(base)&&result.graphSHA256!=base.graphSHA256,"trim changed modification identity");pass("native trim preserves shared modification identity");}
    {auto duplicate=m;auto clip=duplicate.clips[0];clip.nativeListIndex=duplicate.clips.size();clip.nativeClipID20=77;clip.timelineStartTicks00=8*1920;updateRaw(clip);duplicate.clips.push_back(clip);duplicate.nativeRecordCount++;auto result=make(duplicate);demand(mods(result)==mods(base)&&result.graph.regions.size()==7,"native duplicate guessed a separate modification/clone");pass("native duplicate on same channel adds region and preserves exact shared edit object");}
    {auto cold=m;for(auto&clip:cold.clips){clip.nativeClipID20+=100;updateRaw(clip);}for(auto&s:cold.sources){s.waveSampleObject+=0x100000;s.actualWaveSampleVMT+=0x100000;s.nativePCMAddress90+=0x100000;}
        for(auto&c:cold.channels){c.channelObject+=0x100000;c.nativeWaveSampleObject328=cold.sources[c.sourceSnapshotIndex].waveSampleObject;c.actualNativeWaveSampleVMT=cold.sources[c.sourceSnapshotIndex].actualWaveSampleVMT;}
        for(auto&member:cold.nativeManagerMembers){member.actualChannelObject=cold.channels[member.managerIndex].channelObject;member.actualChannelVMT=cold.channels[member.managerIndex].actualChannelVMT;}
        auto result=make(cold);demand(mods(result)==mods(base)&&result.sourcePCMModelSHA256==base.sourcePCMModelSHA256&&result.orderedNativeRecordsSHA256!=base.orderedNativeRecordsSHA256&&result.normalizedOrderedNativeRecordsSHA256==base.normalizedOrderedNativeRecordsSHA256,"cold native renumber lost edit/source identity or masked physical record bytes");
        pass("cold clip IDs/pointers regenerate while channel source/mod IDs stay exact; raw/normalized evidence stays distinct");}
    {auto changed=m;changed.sources[0].pcmBytes[0]^=1;auto result=make(changed);demand(mods(result)!=mods(base)&&result.sourcePCMModelSHA256!=base.sourcePCMModelSHA256,"actual PCM mutation retained old edit/source identity");pass("changed actual source PCM creates distinct source/modification identity");}
    {auto removed=m;removed.clips.clear();removed.nativeRecordCount=0;auto result=make(removed);
        demand(result.graph.regions.empty()&&result.graph.tracks.empty()&&mods(result)==mods(base)&&result.graph.sources.size()==2&&result.sourcePCMModelSHA256==base.sourcePCMModelSHA256
            &&result.actualCopiedNativeModelRecords.empty()&&result.actualVerifiedAudioRecordIndices.empty()&&result.allNativeManagerChannelsInspected&&result.actualNativeManagerChannelCount==2,"last Playlist deletion discarded actual native channel edits or fabricated regions");
        for(const auto&row:result.sources){const auto&native=m.sources[row.nativeChannelIndex];demand(row.actualPCM_SHA256==hash(native.pcmBytes)&&row.sampleCount==native.nativeFrameCount98&&row.channelCount==native.qualifiedChannelCount,"zero-region source evidence differs from actual copied channel PCM");}
        auto undo=make(m);demand(mods(undo)==mods(result)&&undo.graph.regions.size()==6,"Undo did not recover same actual channel edit identities");
        pass("deleting all Playlist regions retains actual channel source/mod IDs and exact copied PCM; Undo restores regions on same edits");}
    {auto removed=m;removed.clips.erase(removed.clips.begin(),removed.clips.begin()+3);for(std::size_t i=0;i<removed.clips.size();++i)removed.clips[i].nativeListIndex=i;removed.nativeRecordCount=int(removed.clips.size());auto result=make(removed);
        demand(result.graph.regions.size()==3&&mods(result)==mods(base)&&result.graph.sources.size()==2,"unreferenced audio channel omitted while another channel retained regions");
        pass("last region removed from one native channel preserves that channel alongside other live regions");}
    {auto other=m;for(auto&channel:other.channels)channel.nativeMixerInsert248=2;for(auto&source:other.sources)source.pcmBytes.clear();other.pcmBytesCopied=0;auto result=make(other);
        demand(result.graph.sources.empty()&&result.graph.modifications.empty()&&result.graph.regions.empty()&&result.actualNativeManagerChannelCount==2&&result.allNativeManagerChannelsInspected,"no-channel selected insert fabricated edit objects");
        pass("complete actual manager inventory permits true no-channel selected insert empty Graph");}
    {auto empty=m;empty.clips.clear();empty.nativeRecordCount=0;empty.channels.clear();empty.sources.clear();empty.nativeManagerMembers.clear();empty.nativeChannelCount=0;empty.pcmBytesCopied=0;auto result=make(empty);
        demand(result.graph.sources.empty()&&result.graph.regions.empty()&&result.actualNativeManagerMembers.empty()&&result.allNativeManagerChannelsInspected,"empty native manager did not produce real empty Graph");
        pass("actual empty manager and empty Playlist produce empty source/mod/region graph with clock/hash authority");}
    reject("zero-region selected channel still requires actual copied PCM",[&]{auto x=m;x.clips.clear();x.nativeRecordCount=0;x.sources[0].pcmBytes.clear();make(x);});
    reject("incomplete native manager inspection cannot authorize an empty Graph",[&]{auto x=m;x.clips.clear();x.nativeRecordCount=0;x.allNativeManagerChannelsInspected=false;make(x);});
    reject("native manager classification extent mismatch refused",[&]{auto x=m;x.nativeManagerMembers.pop_back();make(x);});
    reject("native manager member index out of bounds refused",[&]{auto x=m;x.nativeManagerMembers[1].managerIndex=2;make(x);});
    reject("actual manager audio channel absent from copied snapshot refused",[&]{auto x=m;x.clips.clear();x.nativeRecordCount=0;x.channels.pop_back();make(x);});
    reject("actual native manager channel type mismatch refused",[&]{auto x=m;x.nativeManagerMembers[0].nativeChannelType150=1;make(x);});
    for(bool unknown:{false,true}){auto unrelated=m;unrelated.channels[1].nativeMixerInsert248=2;unrelated.pcmBytesCopied-=unrelated.sources[1].pcmBytes.size();unrelated.sources[1].pcmBytes.clear();
        unrelated.sources[1].nativeFrameCount98=48000LL*60*60*6;unrelated.sources[1].pcmStatus=unknown?"unknown_format_or_residency_no_fallback":"qualified_descriptor_only_outside_selected_insert";
        if(unknown){unrelated.sources[1].qualifiedEncoding=FLNativePCMEncoding::Unknown;unrelated.sources[1].qualifiedChannelCount=0;unrelated.sources[1].nativeFormatByte79=99;}
        FLStandaloneDecoderLimits limits;limits.maxPCMBytesPerSource=unrelated.sources[0].pcmBytes.size();limits.maxPCMBytesTotal=unrelated.sources[0].pcmBytes.size();
        auto result=buildGraph(unrelated,clock,a,sha,limits,false);demand(result.graph.sources.size()==1&&result.sources.size()==1&&result.graph.regions.size()==3&&result.actualNativeManagerMembers.size()==2
            &&result.actualOwnedPCMBytesCopied==unrelated.sources[0].pcmBytes.size()&&result.sources[0].actualPCM_SHA256==hash(unrelated.sources[0].pcmBytes),"unrelated source metadata consumed owned PCM/hash budget or changed selected source evidence");
        pass(unknown?"unrelated six-hour UNKNOWN PCM metadata consumes zero owned copy/hash budget while full manager/rows remain":"unrelated six-hour qualified PCM metadata consumes zero owned copy/hash budget while full manager/rows remain");}
    {auto shared=m;shared.channels[0].nativeMixerInsert248=2;shared.channels[1].sourceSnapshotIndex=0;shared.channels[1].nativeWaveSampleObject328=shared.sources[0].waveSampleObject;
        shared.channels[1].actualNativeWaveSampleVMT=shared.sources[0].actualWaveSampleVMT;shared.sources.resize(1);shared.pcmBytesCopied=shared.sources[0].pcmBytes.size();
        FLStandaloneDecoderLimits limits;limits.maxPCMBytesTotal=shared.pcmBytesCopied;limits.maxPCMBytesPerSource=shared.pcmBytesCopied;
        auto result=buildGraph(shared,clock,a,sha,limits,false);demand(result.sources.size()==1&&result.sources[0].nativeChannelIndex==1&&result.actualOwnedPCMBytesCopied==shared.pcmBytesCopied
            &&result.sources[0].actualPCM_SHA256==hash(shared.sources[0].pcmBytes),"shared actual wave metadata did not admit selected-channel PCM upgrade with a single copy budget");
        pass("actual source shared by earlier unrelated and later selected channels admits one selected PCM payload/hash budget");}
    reject("unrelated PCM allocation refused under selected-insert copy authority",[&]{auto x=m;x.channels[1].nativeMixerInsert248=2;make(x);});
    reject("actual PCM copy byte accounting mismatch refused",[&]{auto x=m;x.pcmBytesCopied++;make(x);});
    reject("PCM copy filter different from actual selected insert refused",[&]{auto x=m;x.pcmCopyFilterMixerInsert=2;make(x);});
    reject("typed raw88 disagreement refused",[&]{auto x=m;x.clips[0].timelineStartTicks00++;make(x);});
    reject("duplicate native audio clip ID refused",[&]{auto x=m;x.clips[1].nativeClipID20=x.clips[0].nativeClipID20;updateRaw(x.clips[1]);make(x);});
    reject("out-of-range source crop refused",[&]{auto x=m;x.clips[0].nativeSourceEnd1C=3000;updateRaw(x.clips[0]);make(x);});
    reject("unsupported source/playback stretch refused",[&]{auto x=m;x.clips[0].timelineLengthTicks08++;updateRaw(x.clips[0]);make(x);});
    reject("missing copied PCM refused without file fallback",[&]{auto x=m;x.sources[0].pcmBytes.clear();make(x);});
    reject("unqualified PCM encoding refused",[&]{auto x=m;x.sources[0].qualifiedEncoding=FLNativePCMEncoding::Float32LE;make(x);});
    reject("unqualified native format byte refused",[&]{auto x=m;x.sources[0].nativeFormatByte79=1;make(x);});
    reject("unqualified C8 source reference refused",[&]{auto x=m;x.sources[0].nativeReferenceC8=1;make(x);});
    reject("unsupported sample rate refused",[&]{auto x=m;x.sources[0].nativeRate88=44100;make(x);});
    reject("unsupported native rate factor refused",[&]{auto x=m;x.sources[0].nativeRateFactor1A0=2;make(x);});
    reject("native channel wave object mismatch refused",[&]{auto x=m;x.channels[0].nativeWaveSampleObject328++;make(x);});
    reject("unsupported native mixer501 refused",[&]{auto x=m;x.channels[0].nativeMixerInsert248=501;make(x);});
    reject("missing repeated-read coherence refused",[&]{auto x=m;x.repeatedReadsMatched=false;make(x);});
    reject("unknown native88 record kind refused",[&]{auto x=m;x.clips[0].kind=FLNativeRecordKind::Unresolved;make(x);});
    reject("native raw88 count mismatch refused",[&]{auto x=m;x.nativeRecordCount--;make(x);});
    reject("unverified component owner association refused",[&]{auto x=a;x.nativePointerAssociationVerified=false;buildGraph(m,clock,x,sha,{},false);});
    reject("direct clock bytes/typed values disagreement refused",[&]{auto c=clock;c.ppq++;buildGraph(m,c,a,sha,{},false);});
    reject("direct clock repeated reads missing refused",[&]{auto c=clock;c.actualDirectNativeReadTwiceAndEqual=false;buildGraph(m,c,a,sha,{},false);});
    reject("unsupported engine digest refused",[&]{auto x=m;x.verifiedArm64SliceSHA256=sha;make(x);});
    reject("per-source PCM copy budget enforced",[&]{FLStandaloneDecoderLimits l;l.maxPCMBytesPerSource=1;buildGraph(m,clock,a,sha,l,false);});
    reject("total PCM copy budget enforced",[&]{FLStandaloneDecoderLimits l;l.maxPCMBytesTotal=1;buildGraph(m,clock,a,sha,l,false);});
    reject("source header hash missing refused",[&]{buildGraph(m,clock,a,"",{},false);});
    std::cout<<"{\"schema\":\"rysup.standalone-fl.decoder-offline-controls/1\",\"pass\":true,\"controls\":"<<passed.size()<<",\"synthetic_input\":true,\"actual_native_capture_or_ara_runtime_proven\":false,\"checks\":[";
    for(std::size_t i=0;i<passed.size();++i){if(i)std::cout<<',';std::cout<<'"'<<passed[i]<<'"';}std::cout<<"]}\n";return 0;
}
} // namespace
} // namespace rysup::ara_lab
int main(){try{return rysup::ara_lab::offlineMain();}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
#endif
