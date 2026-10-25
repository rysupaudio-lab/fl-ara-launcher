// Pure prepared-save comparison/gate controls. No genuine archive is fabricated,
// no coordinator/factory/DC is constructed, and no plug-in/app/SDK call runs.
#include "StandaloneARACoordinator.h"
#include <iostream>

using namespace rysup::ara_lab;
int main(){
    unsigned checks=0,resolverCalls=0;
    const auto require=[&](bool good){++checks;if(!good)throw std::runtime_error("prepared-save pure metadata contract failed");};
    const auto hash=[](char c){return std::string(64,c);};
    StandaloneSnapshotData prepared;
    prepared.actual={"instance","run","insert",123,100,200,hash('a'),hash('b')};
    auto& model=prepared.model;model.nativeProjectIdentity="actual-project-generation";model.nativeEngineSHA256=hash('b');
    model.nativeRecordModelSHA256=hash('c');model.sourcePCMModelSHA256=hash('d');model.graphSHA256=hash('e');model.nativeSaveVersion="actual-native-version";
    model.clock.ppq=960;model.clock.tempoMilliBPM=120000;model.clock.sampleRate=48000;model.clock.nativeSamplePosition=4096;
    model.clock.clockFormatIdentity="actual-native-ppq-tempo";model.clock.actualNativeClockBytes={1,2,3,4,5,6};
    prepared.graph.documentName="Actual model";
    prepared.graph.sources={{"s1","",48000,48000,1},{"s2","",96000,48000,2}};
    prepared.graph.tracks={{"t1",""},{"t2",""}};
    prepared.graph.modifications={{"m1","s1",""},{"m2","s2",""}};
    prepared.graph.regions={{"r1","m1","t1","",0,1,0,1,ARA::kARAPlaybackTransformationNoChanges},
        {"r2","m2","t2","",0,2,4,2,ARA::kARAPlaybackTransformationNoChanges}};
    prepared.sources={{"s1","actual-source-1",hash('1')},{"s2","actual-source-2",hash('2')}};
    prepared.modifications={{"m1","actual-edit-1","s1"},{"m2","actual-edit-2","s2"}};
    prepared.regions={{"r1","actual-region-1","m1"},{"r2","actual-region-2","m2"}};
    prepared.actualCopiedNativeModelRecords.assign(176,5);prepared.actualVerifiedAudioRecordIndices={0,1};
    prepared.decoderIdentity="actual-decoder";prepared.decoderSourceSHA256=hash('f');
    prepared.actualCurrentNativeRAMCopiedAndVerified=true;prepared.actualDirectNativeClockReadTwiceAndEqual=true;
    prepared.nativeComponentOwnerInsertAssociationVerified=true;prepared.noNativeSerializationOrFileFallbackUsed=true;
    prepared.resolveCurrentImmutableRAMSource=[&](const SourceDescriptor&)->std::shared_ptr<SourceProvider>{++resolverCalls;throw std::runtime_error("pure native-save comparison called a resolver");};
    auto current=prepared;current.resolveCurrentImmutableRAMSource={};
    require(standaloneNativeSaveSnapshotMatches(prepared,current));
    auto copy=current;require(standaloneNativeSaveSnapshotMatches(prepared,copy));
    std::swap(copy.graph.sources[0],copy.graph.sources[1]);std::swap(copy.graph.modifications[0],copy.graph.modifications[1]);
    std::swap(copy.graph.regions[0],copy.graph.regions[1]);std::swap(copy.sources[0],copy.sources[1]);
    require(standaloneNativeSaveSnapshotMatches(prepared,copy));
    const auto changed=[&](auto mutate){auto value=current;mutate(value);require(!standaloneNativeSaveSnapshotMatches(prepared,value));};
    changed([](auto& v){v.actual.instanceID="copied-to-another-instance";});
    changed([](auto& v){v.actual.ownRunID="other-run";});
    changed([](auto& v){v.actual.ownProcessID=124;});
    changed([](auto& v){v.actual.actualComponent=101;});
    changed([](auto& v){v.actual.actualNativeEngineOwner=201;});
    changed([](auto& v){v.actual.nativeInsertKey="other-insert";});
    changed([&](auto& v){v.actual.pluginModuleSHA256=hash('3');});
    changed([&](auto& v){v.actual.nativeEngineSHA256=hash('4');});
    changed([](auto& v){v.model.nativeProjectIdentity="other-project";});
    changed([&](auto& v){v.model.nativeRecordModelSHA256=hash('5');});
    changed([&](auto& v){v.model.sourcePCMModelSHA256=hash('6');});
    changed([&](auto& v){v.model.graphSHA256=hash('7');});
    changed([](auto& v){v.model.clock.ppq=480;});
    changed([](auto& v){v.model.clock.tempoMilliBPM=121000;});
    changed([](auto& v){v.model.clock.sampleRate=48001;});
    changed([](auto& v){v.model.clock.actualNativePlaying=true;});
    changed([](auto& v){v.model.clock.actualNativeClockBytes[0]++;});
    changed([](auto& v){v.model.clock.nativeSamplePosition++;});
    copy=current;copy.model.clock.nativeSamplePosition++;
    require(standaloneNativeSaveSnapshotMatches(prepared,copy,false));
    changed([](auto& v){v.actualCopiedNativeModelRecords[31]++;});
    changed([](auto& v){v.actualVerifiedAudioRecordIndices.pop_back();});
    changed([&](auto& v){v.sources[0].actualPCM_SHA256=hash('8');});
    changed([](auto& v){v.sources[0].nativePhysicalSourceKey="other-physical-source";});
    changed([](auto& v){v.modifications[0].nativePhysicalEditKey="other-edit";});
    changed([](auto& v){v.regions[0].nativePhysicalRegionKey="other-native-clip";});
    changed([](auto& v){v.graph.regions[0].playbackStart=1;});
    changed([](auto& v){v.graph.regions[0].sourceDuration=.5;});
    changed([](auto& v){v.graph.regions[0].modificationID="m2";});
    changed([](auto& v){v.graph.sources[0].sampleCount--;});
    changed([](auto& v){v.graph.sources[0].channelCount=2;});
    changed([&](auto& v){v.decoderSourceSHA256=hash('9');});
    changed([](auto& v){v.actualCurrentNativeRAMCopiedAndVerified=false;});
    changed([](auto& v){v.noNativeSerializationOrFileFallbackUsed=false;});
    auto noRegions=current;noRegions.graph.regions.clear();noRegions.graph.tracks.clear();noRegions.regions.clear();
    noRegions.actualCopiedNativeModelRecords.clear();noRegions.actualVerifiedAudioRecordIndices.clear();
    require(standaloneNativeSaveSnapshotMatches(noRegions,noRegions));
    auto empty=noRegions;empty.graph.sources.clear();empty.graph.modifications.clear();empty.sources.clear();empty.modifications.clear();
    require(standaloneNativeSaveSnapshotMatches(empty,empty));
    require(resolverCalls==0);
    StandaloneCacheGate gate;auto ticket=gate.invalidate();require(gate.publishIfUnchanged(ticket));
    require(gate.authorizes(ticket));require(!gate.authorizes({ticket.epoch+1}));
    gate.invalidate();require(!gate.authorizes(ticket));
    auto newer=gate.currentTicket();require(gate.publishIfUnchanged(newer));require(!gate.authorizes(ticket));
    gate.closePermanently();require(!gate.authorizes(newer));
    std::cout<<"{\"pass\":true,\"checks\":"<<checks<<",\"resolverCalls\":"<<resolverCalls<<",\"nativePluginLoaded\":false}\n";
}
