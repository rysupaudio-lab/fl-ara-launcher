#include "FLUnitDeviceGate.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <utility>
using Gate=rysup::fl_ara::test_output::FLUnitDeviceGate;
static unsigned checks=0;
#define REQUIRE(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::abort();}}while(false)
static bool ready(void* p)noexcept{return !p||static_cast<std::atomic<bool>*>(p)->load();}
static void capture(Gate& g,std::uintptr_t unit=1,std::uint32_t dev=7,std::uintptr_t id=9,Gate::Pair pair=Gate::Pair::AUHAL){
 auto scope=g.beginCapture(Gate::Kind::Initialize,unit,dev);REQUIRE(scope);REQUIRE(g.recordCreate(dev,101,id,0,pair));REQUIRE(scope.close());
}
struct RevokeCheck{Gate* gate;std::uintptr_t unit;bool revoked=false;};
static bool revokeInValidator(void* p)noexcept{auto& c=*static_cast<RevokeCheck*>(p);c.revoked=c.gate->revokeUnit(c.unit);return true;}
static void basic(){
 Gate g;REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));
 capture(g);REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));
 std::atomic<bool> armed{true};auto start=g.beginOutputStart(1,7,5,ready,&armed);REQUIRE(start);
 REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));
 REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,true));
 REQUIRE(!g.approveDeviceStart(8,9,Gate::Pair::AUHAL,false));
 REQUIRE(!g.approveDeviceStart(7,0,Gate::Pair::AUHAL,false));
 REQUIRE(!g.approveDeviceStart(7,10,Gate::Pair::AUHAL,false));
 REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::Generic,false));
 REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::None,false));
 armed=false;REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));armed=true;
 REQUIRE(!g.beginOutputStart(1,7,5,ready,&armed));REQUIRE(!g.beginCapture(Gate::Kind::Initialize,2,7));
 REQUIRE(!g.beginCapture(Gate::Kind::Initialize,1,8));
 {auto nested=g.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(nested);REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));}
 REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));
 auto moved=std::move(start);REQUIRE(!start);REQUIRE(moved);REQUIRE(moved.close());
 REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));
 REQUIRE(!g.beginOutputStart(1,7,0,ready,nullptr));REQUIRE(!g.beginOutputStart(1,7,5,nullptr,nullptr));
 REQUIRE(!g.beginCapture(Gate::Kind::OutputStart,1,7));
}
static void crossThread(){
 Gate g;capture(g);auto s=g.beginOutputStart(1,7,1,ready,nullptr);REQUIRE(s);
 bool denied=false,scopeDenied=false,createDenied=false;
 std::thread t([&]{denied=!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false);scopeDenied=!g.beginCapture(Gate::Kind::Initialize,1,7);createDenied=!g.recordCreate(7,101,10,0,Gate::Pair::AUHAL);});t.join();
 REQUIRE(denied&&scopeDenied&&createDenied);REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));REQUIRE(s.close());
 // Destroy/revoke from a different thread invalidates the active association.
 auto active=g.beginOutputStart(1,7,2,ready,nullptr);REQUIRE(active);bool revoked=false;
 std::thread destroy([&]{revoked=g.revokeIOProc(7,9);});destroy.join();REQUIRE(revoked);REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));
}
static void failureAndReuse(){
 Gate g;capture(g);{auto s=g.beginOutputStart(1,7,1,ready,nullptr);REQUIRE(s);REQUIRE(g.originalStartReturned(s,-1));REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));}
 REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));
 auto changed=g.beginCapture(Gate::Kind::CurrentDevice,1,8);REQUIRE(changed);const auto newGen=changed.generation();
 REQUIRE(!g.recordCreate(7,101,9,0,Gate::Pair::AUHAL));REQUIRE(changed.close());
 capture(g,1,8,9);{auto s=g.beginOutputStart(1,8,2,ready,nullptr);REQUIRE(s);REQUIRE(g.approveDeviceStart(8,9,Gate::Pair::AUHAL,false));REQUIRE(g.revokeIOProc(8,9));REQUIRE(!g.approveDeviceStart(8,9,Gate::Pair::AUHAL,false));}
 // Even a failed original Destroy cannot restore revoked metadata.
 {auto s=g.beginOutputStart(1,8,3,ready,nullptr);REQUIRE(s);REQUIRE(!g.approveDeviceStart(8,9,Gate::Pair::AUHAL,false));}
 REQUIRE(g.revokeUnit(1));auto reused=g.beginCapture(Gate::Kind::Initialize,1,8);REQUIRE(reused);REQUIRE(reused.generation()>newGen);
 REQUIRE(!g.recordCreate(8,101,9,-42,Gate::Pair::AUHAL));REQUIRE(reused.close());
 capture(g,1,8,9);{auto s=g.beginOutputStart(1,8,4,ready,nullptr);REQUIRE(s);REQUIRE(g.approveDeviceStart(8,9,Gate::Pair::AUHAL,false));}
}
static void identityPairs(){
 Gate nulls;REQUIRE(!nulls.beginCapture(Gate::Kind::Initialize,0,7));REQUIRE(!nulls.beginCapture(Gate::Kind::Initialize,1,0));
 {auto scope=nulls.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(scope);REQUIRE(!nulls.originalStartReturned(scope,0));REQUIRE(!nulls.recordCreate(7,101,0,0,Gate::Pair::AUHAL));}
 Gate g;{auto scope=g.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(scope);REQUIRE(!g.recordCreate(7,0,9,0,Gate::Pair::AUHAL));}
 capture(g,1,7,9,Gate::Pair::Generic);{auto s=g.beginOutputStart(1,7,1,ready,nullptr);REQUIRE(s);REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::Generic,true));REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::Generic,false));}
 REQUIRE(g.revokeUnit(1));capture(g);
 {auto scope=g.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(scope);REQUIRE(!g.recordCreate(7,102,9,0,Gate::Pair::AUHAL));}
 {auto start=g.beginOutputStart(1,7,1,ready,nullptr);REQUIRE(start);REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));}
 Gate h;capture(h);{auto scope=h.beginCapture(Gate::Kind::Initialize,2,7);REQUIRE(scope);REQUIRE(!h.recordCreate(7,101,9,0,Gate::Pair::AUHAL));}
 {auto start=h.beginOutputStart(1,7,1,ready,nullptr);REQUIRE(start);REQUIRE(!h.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));}
 Gate k;{auto scope=k.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(scope);REQUIRE(!k.recordCreate(7,101,9,0,Gate::Pair::None));}
}
static void boundedAndOrder(){
 Gate g;std::array<Gate::Scope,Gate::maximumScopes> stack;
 for(auto& s:stack){s=g.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(s);}
 REQUIRE(!g.beginCapture(Gate::Kind::Initialize,1,7));for(std::size_t i=stack.size();i;--i)REQUIRE(stack[i-1].close());
 {auto outer=g.beginCapture(Gate::Kind::Initialize,1,7);auto inner=g.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(outer&&inner);REQUIRE(!outer.close());REQUIRE(!inner.close());}
 Gate units;for(std::uintptr_t u=1;u<=Gate::maximumUnits;++u){auto s=units.beginCapture(Gate::Kind::Initialize,u,7);REQUIRE(s);}
 REQUIRE(!units.beginCapture(Gate::Kind::Initialize,100,7));REQUIRE(units.revokeUnit(1));{auto s=units.beginCapture(Gate::Kind::Initialize,100,7);REQUIRE(s);}
 Gate ids;{auto s=ids.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(s);
  for(std::uintptr_t id=1;id<=Gate::maximumIOProcs;++id)REQUIRE(ids.recordCreate(7,101,id,0,Gate::Pair::AUHAL));
  REQUIRE(!ids.recordCreate(7,101,1000,0,Gate::Pair::AUHAL));
 }
 auto s=ids.beginOutputStart(1,7,1,ready,nullptr);REQUIRE(s);REQUIRE(!ids.approveDeviceStart(7,1,Gate::Pair::AUHAL,false));
}
static void validatorBoundary(){
 Gate g;capture(g);RevokeCheck check{&g,1};auto s=g.beginOutputStart(1,7,1,revokeInValidator,&check);REQUIRE(s);
 REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));REQUIRE(check.revoked);REQUIRE(!g.snapshot().poisoned);
}
static void wrongUnitAndDisposedReuse(){
 Gate g;capture(g,1,7,9);capture(g,2,7,10);
 {auto s=g.beginOutputStart(2,7,1,ready,nullptr);REQUIRE(s);REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));REQUIRE(g.approveDeviceStart(7,10,Gate::Pair::AUHAL,false));}
 REQUIRE(g.revokeUnit(1));
 {auto reused=g.beginOutputStart(1,7,2,ready,nullptr);REQUIRE(reused);REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));}
 capture(g,1,7,9);{auto valid=g.beginOutputStart(1,7,3,ready,nullptr);REQUIRE(valid);REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));}
}
static void innerFailureBeforeOuterRetry(){
 Gate g;capture(g);{auto metadata=g.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(metadata);REQUIRE(g.recordCreate(7,101,10,0,Gate::Pair::AUHAL));}
 REQUIRE(!g.deviceStartReturned(7,9,-1)); // Outside the actual OutputStart stack.
 auto outer=g.beginOutputStart(1,7,5,ready,nullptr);REQUIRE(outer);
 REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));REQUIRE(g.deviceStartReturned(7,9,0));
 REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));REQUIRE(g.deviceStartReturned(7,9,-42));
 REQUIRE(!g.approveDeviceStart(7,10,Gate::Pair::AUHAL,false)); // Same stack, different captured ID.
 REQUIRE(!g.deviceStartReturned(7,9,0)); // A later success cannot revive.
 REQUIRE(g.originalStartReturned(outer,0));REQUIRE(!g.approveDeviceStart(7,10,Gate::Pair::AUHAL,false));REQUIRE(outer.close());
 auto next=g.beginOutputStart(1,7,6,ready,nullptr);REQUIRE(next);REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));REQUIRE(!g.approveDeviceStart(7,10,Gate::Pair::AUHAL,false));
}
static void destructionLeases(){
 Gate g;capture(g);auto start=g.beginOutputStart(1,7,1,ready,nullptr);REQUIRE(start);
 REQUIRE(!g.beginDestroy(8,9));REQUIRE(!g.beginDestroy(7,0));
 auto pending=g.beginDestroy(7,9);REQUIRE(pending);REQUIRE(pending.known());REQUIRE(pending.unit()==1);REQUIRE(pending.generation()==start.generation());
 REQUIRE(!g.beginDestroy(7,9));REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));
 auto moved=std::move(pending);REQUIRE(!pending);REQUIRE(moved);REQUIRE(moved.close());
 REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false)); // Busy Sink can abandon without revocation.
 auto destroyed=g.beginDestroy(7,9);REQUIRE(destroyed);REQUIRE(!moved.commitRevoke());REQUIRE(destroyed.commitRevoke());REQUIRE(!destroyed.commitRevoke());
 REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));REQUIRE(destroyed.close());
 REQUIRE(!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false)); // Failed native destroy stays revoked.
 REQUIRE(start.close());capture(g);{auto s=g.beginOutputStart(1,7,2,ready,nullptr);REQUIRE(s);REQUIRE(g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));}
 REQUIRE(g.revokeUnit(1));auto dead=g.beginDestroy(7,9);REQUIRE(dead);REQUIRE(dead.known());REQUIRE(dead.unit()==1);REQUIRE(dead.commitRevoke());REQUIRE(dead.close());
 auto unknown=g.beginDestroy(7,77);REQUIRE(unknown);REQUIRE(!unknown.known());REQUIRE(unknown.unit()==0);
 {auto scope=g.beginCapture(Gate::Kind::Initialize,2,7);REQUIRE(scope);REQUIRE(!g.recordCreate(7,101,77,0,Gate::Pair::AUHAL));}
 REQUIRE(unknown.commitRevoke());REQUIRE(unknown.close());capture(g,2,7,77);{auto s=g.beginOutputStart(2,7,3,ready,nullptr);REQUIRE(s);REQUIRE(g.approveDeviceStart(7,77,Gate::Pair::AUHAL,false));}
 auto cross=g.beginDestroy(7,77);REQUIRE(cross);bool foreignCommit=true;std::thread t([&]{foreignCommit=cross.commitRevoke();});t.join();REQUIRE(!foreignCommit);REQUIRE(cross.close());
}
static void concurrentDestroyAndReservationPools(){
 Gate g;capture(g);std::atomic<unsigned> phase{0};bool scopeOK=false,blocked=false,afterAbandon=false;
 std::thread worker([&]{auto s=g.beginOutputStart(1,7,1,ready,nullptr);scopeOK=static_cast<bool>(s);phase=1;
  while(phase.load()<2)std::this_thread::yield();blocked=!g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false);phase=3;
  while(phase.load()<4)std::this_thread::yield();afterAbandon=g.approveDeviceStart(7,9,Gate::Pair::AUHAL,false);phase=5;
 });
 while(phase.load()<1)std::this_thread::yield();auto destroy=g.beginDestroy(7,9);REQUIRE(destroy);
 {auto captureOther=g.beginCapture(Gate::Kind::Initialize,2,7);REQUIRE(captureOther);REQUIRE(!g.recordCreate(7,101,9,0,Gate::Pair::AUHAL));}
 phase=2;while(phase.load()<3)std::this_thread::yield();REQUIRE(destroy.close());phase=4;worker.join();REQUIRE(scopeOK&&blocked&&afterAbandon);
 Gate pool;std::array<Gate::DestructionLease,Gate::maximumDestructions> leases;
 for(std::size_t i=0;i<leases.size();++i){leases[i]=pool.beginDestroy(7,100+i);REQUIRE(leases[i]);REQUIRE(!leases[i].known());}
 REQUIRE(!pool.beginDestroy(7,999));REQUIRE(leases[0].close());auto replacement=pool.beginDestroy(7,999);REQUIRE(replacement);REQUIRE(!leases[0].commitRevoke());REQUIRE(replacement.commitRevoke());REQUIRE(replacement.close());
 Gate history;capture(history);REQUIRE(history.revokeUnit(1));capture(history,1,7,10);
 auto oldBinding=history.beginDestroy(7,9);REQUIRE(oldBinding);REQUIRE(oldBinding.known());REQUIRE(oldBinding.unit()==1);
 {auto metadata=history.beginCapture(Gate::Kind::Initialize,1,7);REQUIRE(metadata);REQUIRE(oldBinding.generation()!=metadata.generation());}
 REQUIRE(oldBinding.commitRevoke());REQUIRE(oldBinding.close());
 {auto s=history.beginOutputStart(1,7,1,ready,nullptr);REQUIRE(s);REQUIRE(!history.approveDeviceStart(7,9,Gate::Pair::AUHAL,false));REQUIRE(history.approveDeviceStart(7,10,Gate::Pair::AUHAL,false));}
}
int main(){basic();crossThread();failureAndReuse();identityPairs();boundedAndOrder();validatorBoundary();wrongUnitAndDisposedReuse();innerFailureBeforeOuterRetry();destructionLeases();concurrentDestroyAndReservationPools();std::printf("PASS SDK-free UnitDeviceGate %u controls; no Audio/CoreAudio/SDK/device/native app APIs\n",checks);}
