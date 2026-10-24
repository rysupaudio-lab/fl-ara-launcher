#!/usr/bin/env python3
"""Compile public contracts and run ONLY the pure atomic cache gate; no plug-ins/apps."""
from pathlib import Path
import hashlib
import json
import os
import subprocess

SOURCE = Path(__file__).resolve().parent
ARA = Path(os.environ.get('ARA_SDK_ROOT', '/missing/ARA_SDK_ROOT')).resolve()
VST = Path(os.environ.get('VST3_SDK_ROOT', '/missing/VST3_SDK_ROOT')).resolve()
OUT = Path(os.environ.get('FL_CONTRACT_OUTPUT', '/missing/FL_CONTRACT_OUTPUT')).resolve()

def main():
    if not all(os.environ.get(k) for k in ['ARA_SDK_ROOT','VST3_SDK_ROOT','FL_CONTRACT_OUTPUT']):
        raise SystemExit('Set external ARA_SDK_ROOT, VST3_SDK_ROOT and FL_CONTRACT_OUTPUT before running checks')
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/'tmp').mkdir(exist_ok=True)
    env=dict(os.environ,TMPDIR=str(OUT/'tmp'))
    flags=['-std=c++17','-Wall','-Wextra','-Werror','-DARA_SUPPORT_VERSION_1=0','-I',str(SOURCE),'-I',str(ARA),'-I',str(VST)]
    prefix='#include "StandaloneARACoordinator.h"\nusing namespace rysup::ara_lab;\n'
    cases=[
        ('reject-authored-empty-snapshot',False,prefix+'void check(){CapturedStandaloneSnapshot fake;}\n'),
        ('reject-authored-graph-snapshot',False,prefix+'void check(Graph graph){CapturedStandaloneSnapshot fake(graph);}\n'),
        ('reject-mutating-captured-native-graph',False,prefix+'void check(const CapturedStandaloneSnapshot& actual){actual.data().graph.sources.clear();}\n'),
        ('reject-graph-only-refresh',False,prefix+'void check(StandaloneARACoordinator& c,const std::string& id,Graph graph){c.refreshGraph(id,graph);}\n'),
        ('reject-au-fiction',False,prefix+'void check(StandaloneARACoordinator& c){c.captureOwnedNativeClassInfo(nullptr,1,true);}\n'),
        ('positive-actual-capture-refresh-cache-api',True,prefix+'''
void check(StandaloneARACoordinator& c,const std::string& id){
    auto actual=c.captureActualCurrentSnapshot(id);
    c.refreshGraph(id,actual);
    c.pollAtStoppedNativeBoundary(id);
    auto batch=c.rebuildWholeSourceCaches(id,actual,WholeSourceCacheOptions{});
    bool current=batch.gate->authorizes(batch.ticket);(void)current;
    c.invalidateSourceCachesAndRequestRebuild(id);
}
'''),
        ('positive-same-dc-refresh-clone-replace-api',True,prefix+'''
void check(HostController& host,const Graph& actual,const HostController::ResolveSource& ram,
           const ArchiveSnapshot& archive,const ArchiveObjectIDRelocation& map){
    host.reconcileEditorGraphWhileActive(actual,ram,{{"new-native-mod","existing-native-mod"}});
    host.replaceEditorGraphWhileActive(actual,ram,&archive,&map);
    host.replaceEditorGraphWhileActive(actual,ram);
}
'''),
        ('positive-explicit-existing-bound-archive-application',True,prefix+'''
void check(StandaloneARACoordinator& c,const std::string& id,const StandaloneSavedEditorArchive& archive,
           StandaloneArchiveSourceValidator& actualNativeLoad){
    auto r=c.restoreArchiveBeforeOriginalState(id,archive,actualNativeLoad,
        StandaloneArchiveApplication::ExistingBoundNativeProjectReplacement);
    (void)r;
    c.observeOriginalSetStateResult(id,Steinberg::kResultOk);
}
'''),
        ('reject-implicit-archive-application-mode',False,prefix+'''
void check(StandaloneARACoordinator& c,const std::string& id,const StandaloneSavedEditorArchive& archive,
           StandaloneArchiveSourceValidator& actual){c.restoreArchiveBeforeOriginalState(id,archive,actual);}
'''),
        ('positive-owned-call-scope-integration',True,prefix+'''#include "OwnedVST3PlaybackCompanion.h"
bool check(){return ownedPlaybackCompanionCallOnThisThread();}
'''),
        ('positive-explicit-original-ordinary-parameter-state',True,prefix+'''
void check(WholeSourceCacheOptions& options,const Bytes& actualOriginalOnlyState){
    options.ordinaryCompanionStateBeforeActivation=actualOriginalOnlyState;
    options.applyOrdinaryCompanionStateBeforeActivation=true;
}
'''),
        ('positive-actual-audio-record-classification-extension',True,prefix+'''
void check(StandaloneSnapshotData& current,StandaloneSavedEditorArchive& saved){
    current.actualVerifiedAudioRecordIndices={0,2,7};
    saved.actualVerifiedAudioRecordIndices=current.actualVerifiedAudioRecordIndices;
}
'''),
        ('positive-fresh-authority-defaults-closed',True,prefix+'''
void check(){StandaloneFreshNativeInitialization fresh{};
    if(fresh.originalInitializeSucceeded || fresh.actualNativeComponentFactoryAndOwnerVerified
    || fresh.actualLoadedComponentAndFactoryImageVerified || fresh.beforeFirstNativeStateActivationProcessingContextOrEditor
    || fresh.nativeOwnerDoesNotAlreadyInitializeARA)throw std::runtime_error("native binding authority must default closed");}
'''),
        ('positive-last-moment-borrowed-native-owner-freshness',True,prefix+'''#include "VST3ARAEntry.h"
void check(const bool& actualStillFresh){StandaloneFreshNativeInitialization proof;
    proof.verifyActualBorrowedNativeFreshnessImmediatelyBeforeBinding=[&]{return actualStillFresh;};
    VST3ARAEntry::NativeLifecycle native;
    native.verifyBorrowedNativeFreshnessImmediatelyBeforeBind=proof.verifyActualBorrowedNativeFreshnessImmediatelyBeforeBinding;}
'''),
        ('positive-component-only-teardown-authority-default-closed',True,prefix+'''
constexpr StandaloneNativeTeardownBoundary closed{};
static_assert(!closed.authorizesKnownComponent(123),"old adapters must not authorize teardown");
void check(StandaloneOwnerAdapter& owner,const StandaloneInstanceIdentity& actual){
    StandaloneNativeTeardownBoundary boundary=owner.verifyActualNativeTeardownBoundary(actual);
    bool ready=boundary.authorizesKnownComponent(actual.actualComponent);(void)ready;
}
'''),
        ('reject-model-hold-as-teardown-argument',False,prefix+'''
void check(StandaloneARACoordinator& c,const std::string& id,const StandaloneNativeHold& held){c.prepareForOriginalTerminate(id,held);}
'''),
        ('positive-typed-actual-analysis-retry',True,prefix+'''
void check(){try{throw SourceCacheNotReady("actual analysis incomplete");}catch(const SourceCacheNotReady&){} }
'''),
        ('positive-empty-or-unreferenced-model-cache-plan',True,prefix+'''
void check(Graph actual,StandaloneSourceCacheBatch& batch){
    const auto plan=planStandaloneSourceCaches(actual);
    batch.noSourceOverlayRequired=plan.noSourceOverlayRequired;
    batch.preservedUnreferencedModificationIDs=plan.preservedUnreferencedModificationIDs;
}
'''),
        ('positive-typed-native-preflight-boundary-defer',True,prefix+'''
class NativeMixBoundaryDeferred:public NativeModelBoundaryUnavailable {
public:using NativeModelBoundaryUnavailable::NativeModelBoundaryUnavailable;
};
void check(){try{throw NativeMixBoundaryDeferred("actual native worker mix EBUSY");}
    catch(const NativeModelBoundaryUnavailable&){} }
'''),
    ]
    receipts=[]
    for name,expected,body in cases:
        path=OUT/(name+'.cpp');path.write_text(body)
        command=['clang++',*flags,'-fsyntax-only',str(path)]
        result=subprocess.run(command,env=env,capture_output=True,text=True)
        receipts.append({'name':name,'pass':(result.returncode==0)==expected,'expectedCompileSuccess':expected,
                         'observedCompileSuccess':result.returncode==0,'command':command,'stderr':result.stderr})
    cpp=[SOURCE/name for name in ('StandaloneARACoordinator.cpp','GenericARAHost.cpp','VST3ARAEntry.cpp','OwnedVST3PlaybackCompanion.cpp')]
    command=['clang++',*flags,'-fsyntax-only',*[str(p) for p in cpp]]
    result=subprocess.run(command,env=env,capture_output=True,text=True)
    receipts.append({'name':'strict-current-source-compile','pass':result.returncode==0,'command':command,'stderr':result.stderr})
    gate=OUT/'pure-cache-gate.cpp'
    gate.write_text(prefix+'''#include <cassert>
#include <thread>
#include <iostream>
int main(){
    StandaloneCacheGate gate;unsigned checks=0;
    auto require=[&](bool value){++checks;if(!value)std::abort();};
    auto original=gate.currentTicket();require(!gate.authorizes(original));
    require(gate.publishIfUnchanged(original));require(gate.authorizes(original));
    auto first=gate.invalidate();require(first.epoch>original.epoch);
    require(!gate.authorizes(original));require(!gate.publishIfUnchanged(original));
    require(gate.publishIfUnchanged(first));require(gate.authorizes(first));
    require(!gate.publishIfUnchanged(first));
    std::thread edit([&]{for(int i=0;i<1000;++i)gate.invalidate();});edit.join();
    require(!gate.authorizes(first));auto latest=gate.currentTicket();
    require(latest.epoch==first.epoch+1000);require(gate.publishIfUnchanged(latest));require(gate.authorizes(latest));
    gate.closePermanently();require(!gate.authorizes(latest));
    require(!gate.publishIfUnchanged(gate.currentTicket()));
    StandaloneNativeTeardownBoundary closed;require(!closed.authorizesKnownComponent(123));
    StandaloneNativeTeardownBoundary actual;
    actual.actualComponent=123;actual.exactKnownOwnedComponentVerified=true;
    actual.insideSynchronousNativeBeforeOriginalTerminate=true;actual.onActualModelThread=true;
    actual.actualOriginalSetActiveFalseObserved=true;actual.actualOriginalSetProcessingFalseObserved=true;
    actual.ownNativeProcessGateClosed=true;actual.actualOriginalProcessCallsInFlight=0;
    actual.nativeOriginalTerminateNotYetCalled=true;
    require(actual.authorizesKnownComponent(123));require(!actual.authorizesKnownComponent(124));
    auto changed=actual;changed.exactKnownOwnedComponentVerified=false;require(!changed.authorizesKnownComponent(123));
    changed=actual;changed.insideSynchronousNativeBeforeOriginalTerminate=false;require(!changed.authorizesKnownComponent(123));
    changed=actual;changed.onActualModelThread=false;require(!changed.authorizesKnownComponent(123));
    changed=actual;changed.actualOriginalSetActiveFalseObserved=false;require(!changed.authorizesKnownComponent(123));
    changed=actual;changed.actualOriginalSetProcessingFalseObserved=false;require(!changed.authorizesKnownComponent(123));
    changed=actual;changed.ownNativeProcessGateClosed=false;require(!changed.authorizesKnownComponent(123));
    changed=actual;changed.actualOriginalProcessCallsInFlight=1;require(!changed.authorizesKnownComponent(123));
    changed=actual;changed.nativeOriginalTerminateNotYetCalled=false;require(!changed.authorizesKnownComponent(123));
    Graph empty;auto plan=planStandaloneSourceCaches(empty);
    require(plan.noSourceOverlayRequired&&plan.renderModificationIDs.empty()&&plan.preservedUnreferencedModificationIDs.empty());
    Graph retained;retained.sources={{"source","",48000,48000,1}};
    retained.modifications={{"existing-edit-A","source",""},{"existing-edit-B","source",""}};
    plan=planStandaloneSourceCaches(retained);
    require(plan.noSourceOverlayRequired&&plan.renderModificationIDs.empty()&&plan.preservedUnreferencedModificationIDs.size()==2);
    retained.tracks={{"track",""}};retained.regions={{"clip-A","existing-edit-A","track","",0,1,0,1,ARA::kARAPlaybackTransformationNoChanges},
        {"clip-B","existing-edit-A","track","",0,1,2,1,ARA::kARAPlaybackTransformationNoChanges}};
    plan=planStandaloneSourceCaches(retained);
    require(!plan.noSourceOverlayRequired&&plan.renderModificationIDs==std::vector<std::string>{"existing-edit-A"}
        &&plan.preservedUnreferencedModificationIDs==std::vector<std::string>{"existing-edit-B"});
    retained.regions.clear();plan=planStandaloneSourceCaches(retained);
    require(plan.noSourceOverlayRequired&&plan.renderModificationIDs.empty()&&retained.modifications.size()==2);
    retained.regions={{"bad","missing-edit","track","",0,1,0,1,ARA::kARAPlaybackTransformationNoChanges}};
    bool bad=false;try{planStandaloneSourceCaches(retained);}catch(const std::invalid_argument&){bad=true;}require(bad);
    std::cout<<"{\\\"pass\\\":true,\\\"checks\\\":"<<checks<<",\\\"nativePluginLoaded\\\":false}\\n";
}
''')
    binary=OUT/'pure-cache-gate'
    command=['clang++',*flags,'-O1',str(gate),'-o',str(binary)]
    result=subprocess.run(command,env=env,capture_output=True,text=True)
    receipts.append({'name':'pure-gate-build','pass':result.returncode==0,'command':command,'stderr':result.stderr})
    if result.returncode==0:
        result=subprocess.run([str(binary)],env=env,capture_output=True,text=True)
        gate_result=json.loads(result.stdout) if result.returncode==0 else {}
        receipts.append({'name':'pure-gate-runtime-no-plugin','pass':result.returncode==0 and gate_result.get('checks')==31,
                         'result':gate_result,'stdout':result.stdout,'stderr':result.stderr})
    fresh_binary=OUT/'last-bind-freshness'
    command=['clang++',*flags,'-O1',str(SOURCE/'check_last_bind_freshness.cpp'),str(SOURCE/'VST3ARAEntry.cpp'),'-o',str(fresh_binary)]
    result=subprocess.run(command,env=env,capture_output=True,text=True)
    receipts.append({'name':'offline-last-bind-freshness-build','pass':result.returncode==0,'command':command,'stderr':result.stderr})
    if result.returncode==0:
        result=subprocess.run([str(fresh_binary)],env=env,capture_output=True,text=True)
        observed=json.loads(result.stdout) if result.returncode==0 else {}
        receipts.append({'name':'offline-last-bind-freshness-runtime','pass':result.returncode==0 and observed.get('checks')==5,
                         'result':observed,'stdout':result.stdout,'stderr':result.stderr})
    sources=[*cpp,*[SOURCE/name for name in ('StandaloneARACoordinator.h','StandaloneCacheGate.h','GenericARAHost.h','VST3ARAEntry.h','OwnedVST3PlaybackCompanion.h')]]
    receipt={'schema':'rysup.standalone-ara-owner-neutral-contracts/1','pass':all(r['pass'] for r in receipts),
             'checks':receipts,'sourceSeals':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sources],
             'nativePluginLoaded':False,'normalFLQualified':False,'hardwareUsed':False}
    (OUT/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({'pass':receipt['pass'],'checks':len(receipts),'receipt':str(OUT/'receipt.json'),'nativePluginLoaded':False},indent=2))
    for row in receipts:
        if not row['pass']:print(row['name'],row.get('stderr',''))
    if not receipt['pass']:raise SystemExit(1)

if __name__=='__main__':main()
