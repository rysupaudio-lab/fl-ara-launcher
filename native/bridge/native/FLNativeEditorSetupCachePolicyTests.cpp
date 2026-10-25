// SDK-free adapter simulations. Exercises the SAME atomics/gate update policy
// invoked by FLAppBridge::setup and the real shared edit-invalidation helpers.
// No native component, ARA factory, application or audio device is loaded.
#include "FLNativeEditorSetupCachePolicy.h"
#include "../host/StandaloneCacheInvalidation.h"
#include "../host/StandaloneARAEditRelay.h"
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <string>

namespace policy=rysup::fl_ara::editor_setup_cache;
namespace lab=rysup::ara_lab;
namespace {
std::vector<std::string> passed;
void demand(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void pass(const char* message){passed.emplace_back(message);}
struct EditorOnlyAdapter {
    std::atomic<double> rate{48000};std::atomic<bool> qualified{true},dirty{false};
    lab::StandaloneCacheGate gate;lab::StandaloneCacheTicket cachedTicket;
    lab::StandaloneARAEditRelay actualEditRelay;
    std::atomic<bool> stopRebuildQueued{false};std::atomic<std::uint64_t> dirtyRevision{0};
    std::atomic<unsigned> authorityReads{0};
    bool alive=true,disabled=false,fault=false,teardown=false,graphReady=true,initialPrimePending=false,pendingArchive=false,prepared=true;
    // Independent full-source companion/cache identities never change merely
    // because the borrowed editor component changes its own ProcessSetup.
    const std::uint64_t preparedSourceGeneration=42;
    const std::int32_t ownedOfflineSourceBlock=512;
    EditorOnlyAdapter(){cachedTicket=gate.invalidate();demand(gate.publishIfUnchanged(cachedTicket),"fixture source-cache publication failed");}
    bool coherent()noexcept{
        authorityReads.fetch_add(1);
        return alive&&!disabled&&!fault&&!teardown&&qualified.load()&&graphReady&&!dirty.load()&&!initialPrimePending&&!pendingArchive&&prepared&&gate.authorizes(cachedTicket);
    }
    policy::Decision observe(policy::ActualNativeSetup value,bool mayReadAuthority=true)noexcept{
        return policy::apply(value,mayReadAuthority,rate,qualified,dirty,&gate,[&]()noexcept{return coherent();});
    }
    void actualEdit(std::uint64_t revision)noexcept{
        lab::invalidateStandaloneCacheForEdit(gate,revision,
            [](std::uint64_t)noexcept{},
            [&](std::uint64_t)noexcept{stopRebuildQueued=true;},
            [&](std::uint64_t value)noexcept{actualEditRelay.forwardActualRevision(value,[&](std::uint64_t latest)noexcept{dirtyRevision=latest;});},
            [&]()noexcept{dirty=true;});
    }
};
policy::ActualNativeSetup supported(std::int32_t mode=0,std::int32_t block=128){return {true,48000,0,mode,block};}

void actualC9Sequence(){
    EditorOnlyAdapter a;const auto epoch=a.gate.currentTicket().epoch;
    const std::array<policy::ActualNativeSetup,5> actualSequence{supported(0,128),supported(0,4096),supported(2,4096),supported(0,4096),supported(0,128)};
    for(const auto& setup:actualSequence){const auto result=a.observe(setup);
        demand(result.action==policy::Action::PreserveCoherentSourceCache&&result.setupQualified&&a.qualified&&a.rate==48000,
            "successful qualified native processing-mode/block transition was not preserved");
        demand(a.gate.currentTicket().epoch==epoch&&a.gate.authorizes(a.cachedTicket)&&!a.dirty&&
            !a.initialPrimePending&&!a.stopRebuildQueued&&a.preparedSourceGeneration==42&&a.ownedOfflineSourceBlock==512,
            "native editor configuration changed independent prepared source authority/dirty/prime");}
    demand(a.authorityReads==actualSequence.size(),"preservation did not query current full authority each time");
    pass("actual C9 128 -> 4096 -> offline2 -> realtime0 -> 128 sequence preserves coherent cache epoch/dirty/prime and separate owned-source configuration");
    for(const auto mode:{0,1,2})demand(a.observe(supported(mode,65536)).action==policy::Action::PreserveCoherentSourceCache,"legal bounded processing mode was refused");
    pass("all legal SDK modes and positive existing native frame bound are policy-admitted without granting runtime coverage");
}
template<class Change>void strictCase(const char* name,Change change,bool qualifiedAfter){
    EditorOnlyAdapter a;auto setup=supported();change(a,setup);const auto epoch=a.gate.currentTicket().epoch;
    const auto result=a.observe(setup);
    demand(result.action==policy::Action::InvalidateForRebuild&&result.setupQualified==qualifiedAfter&&a.qualified.load()==qualifiedAfter,
        "unready/incompatible/failed setup did not retain strict qualification result");
    demand(a.dirty&&a.gate.currentTicket().epoch==epoch+1&&!a.gate.authorizes(a.cachedTicket),
        "unready/incompatible/failed setup did not close actual cache authority and queue dirty");
    pass(name);
}
void strictControls(){
    strictCase("first previously unqualified setup still invalidates/requires actual owner priming",[](auto& a,auto&){a.qualified=false;},true);
    strictCase("initial prime pending is never cleared by supported reconfiguration",[](auto& a,auto&){a.initialPrimePending=true;},true);
    strictCase("already-dirty native/ARA edit cannot be cleared by supported setup",[](auto& a,auto&){a.dirty=true;},true);
    strictCase("missing applied graph retains strict rebuild",[](auto& a,auto&){a.graphReady=false;},true);
    strictCase("missing prepared source-cache save authority retains strict rebuild",[](auto& a,auto&){a.prepared=false;},true);
    strictCase("pending native archive retains strict rebuild",[](auto& a,auto&){a.pendingArchive=true;},true);
    strictCase("disabled owner retains strict invalidation",[](auto& a,auto&){a.disabled=true;},true);
    strictCase("tearing-down owner cannot preserve a source cache",[](auto& a,auto&){a.teardown=true;},true);
    strictCase("stale source ticket is never revived by successful setup",[](auto& a,auto&){a.gate.invalidate();},true);
    strictCase("permanently closed source gate remains closed",[](auto& a,auto&){a.gate.closePermanently();},true);
    strictCase("original native setup failure remains unsupported and invalidates",[](auto&,auto& s){s.originalSucceeded=false;},false);
    strictCase("44.1k native setup cannot retain the 48k source route",[](auto&,auto& s){s.sampleRate=44100;},false);
    strictCase("float64 native precision cannot retain the float32 source route",[](auto&,auto& s){s.symbolicSampleSize=1;},false);
    strictCase("unknown native sample precision is refused",[](auto&,auto& s){s.symbolicSampleSize=99;},false);
    strictCase("negative unknown native process mode is refused",[](auto&,auto& s){s.processMode=-1;},false);
    strictCase("unknown native process mode above SDK modes is refused",[](auto&,auto& s){s.processMode=3;},false);
    strictCase("zero native block extent is refused",[](auto&,auto& s){s.maxSamplesPerBlock=0;},false);
    strictCase("negative native block extent is refused",[](auto&,auto& s){s.maxSamplesPerBlock=-1;},false);
    strictCase("unbounded native block extent is refused",[](auto&,auto& s){s.maxSamplesPerBlock=65537;},false);
    strictCase("nonfinite native sample rate is refused without normalization",[](auto&,auto& s){s.sampleRate=std::numeric_limits<double>::quiet_NaN();},false);
    strictCase("infinite native sample rate is refused",[](auto&,auto& s){s.sampleRate=std::numeric_limits<double>::infinity();},false);
    EditorOnlyAdapter a;const auto epoch=a.gate.currentTicket().epoch;
    const auto result=a.observe(supported(),false);
    demand(result.action==policy::Action::InvalidateForRebuild&&a.authorityReads==0&&a.dirty&&a.gate.currentTicket().epoch==epoch+1,
        "off-model/inside-process setup queried unsafe model authority");
    pass("off-model or inside-native-process setup performs atomic invalidation only and never reads model authority");
    std::atomic<double> rate{48000};std::atomic<bool> qualified{true},dirty{false};unsigned queries=0;
    const auto missing=policy::apply<lab::StandaloneCacheGate>(supported(),true,rate,qualified,dirty,nullptr,[&]()noexcept{++queries;return true;});
    demand(missing.action==policy::Action::InvalidateForRebuild&&dirty&&queries==0,"missing actual gate was treated as authorized");
    pass("no actual cache gate cannot enter preservation or query a fictitious source authority");
}
void concurrentEditControls(){
    {
        EditorOnlyAdapter a;const auto before=a.gate.currentTicket().epoch;
        const auto decision=policy::apply(supported(2,4096),true,a.rate,a.qualified,a.dirty,&a.gate,[&]()noexcept{
            const bool wasCoherent=a.coherent();a.actualEdit(7);return wasCoherent;
        });
        demand(decision.action==policy::Action::PreserveCoherentSourceCache&&a.gate.currentTicket().epoch>before&&!a.gate.authorizes(a.cachedTicket)&&
            a.dirty&&a.stopRebuildQueued&&a.dirtyRevision==7&&!a.initialPrimePending,
            "actual edit after coherence read was lost/reopened/cleared by preservation");
        pass("deterministic edit AFTER coherent snapshot stays gate-closed/dirty/STOP queued through preservation");
    }
    {
        EditorOnlyAdapter a;a.actualEdit(8);const auto afterEdit=a.gate.currentTicket().epoch;
        const auto result=a.observe(supported(0,4096));
        demand(result.action==policy::Action::InvalidateForRebuild&&a.gate.currentTicket().epoch==afterEdit+1&&a.dirty&&a.stopRebuildQueued&&a.dirtyRevision==8,
            "actual edit before setup lost its rebuild request");
        pass("edit BEFORE setup retains strict closed-cache path and the original edit revision/mailboxes");
    }
    {
        EditorOnlyAdapter a;std::atomic<bool> begin{false};constexpr std::uint64_t edits=1000;
        std::thread notify([&]{while(!begin.load())std::this_thread::yield();for(std::uint64_t revision=1;revision<=edits;++revision)a.actualEdit(revision);});
        begin=true;for(unsigned i=0;i<10000;++i)a.observe(supported(i%3,(i&1)?128:4096));notify.join();
        demand(a.dirty&&a.stopRebuildQueued&&a.dirtyRevision==edits&&a.actualEditRelay.lastForwardedActualRevision()==edits&&!a.gate.authorizes(a.cachedTicket),
            "concurrent edit/setup stress lost or revived current source authority");
        pass("1000 real shared-helper edit invalidations racing 10000 native setup decisions lose no revision/dirty request and never revive stale ticket");
    }
}
}
int main(){try{
    actualC9Sequence();strictControls();concurrentEditControls();
    std::cout<<"{\"schema\":\"rysup.fl-native-editor-setup-cache-policy/1\",\"pass\":true,\"checks\":"<<passed.size()
        <<",\"sdk_free\":true,\"native_or_ara_loaded\":false,\"native_runtime_qualification\":false,\"controls\":[";
    for(std::size_t i=0;i<passed.size();++i){if(i)std::cout<<',';std::cout<<'"'<<passed[i]<<'"';}std::cout<<"]}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
