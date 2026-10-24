#include "FLNativeProcessObserver.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <iostream>
static std::atomic<unsigned> allocations{0};
void* operator new(std::size_t n){allocations.fetch_add(1);if(void* p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
using namespace rysup::fl_ara;
using namespace Steinberg;
using namespace Steinberg::Vst;
struct Fixture {
    native_hooks::InstanceRecord record;
    process_observer::NativeProcessObserver observer;
    float left[4]{0.25f,-0.5f,0.75f,-1.0f},right[4]{-0.25f,0.5f,-0.75f,1.0f};
    float outLeft[4]{0.25f,-0.5f,0.75f,-1.0f},outRight[4]{-0.25f,0.5f,-0.75f,1.0f};
    float* in[2]{left,right};float* out[2]{outLeft,outRight};
    AudioBusBuffers input,output;ProcessData data;
    native_hooks::ThreadInfo thread;
    Fixture():observer(record){
        record.actualProcessor.store(reinterpret_cast<IAudioProcessor*>(std::uintptr_t{0x12340000}));
        input.numChannels=output.numChannels=2;input.channelBuffers32=in;output.channelBuffers32=out;
        data.numInputs=data.numOutputs=1;data.inputs=&input;data.outputs=&output;data.numSamples=4;
        thread.nativeThreadToken=1;thread.insideNativeProcess=true;
    }
    void before(){native_hooks::NativeProcessTelemetry t{native_hooks::NativeProcessTelemetryPhase::BeforeOriginal,record.actualProcessor.load(),&data,false,kResultOk};observer.observe(record,t,thread);}
    void after(tresult result=kResultOk){native_hooks::NativeProcessTelemetry t{native_hooks::NativeProcessTelemetryPhase::AfterOriginal,record.actualProcessor.load(),&data,true,result};observer.observe(record,t,thread);}
    process_observer::Snapshot stats(){process_observer::Snapshot s;assert(observer.trySnapshot(s));return s;}
};
int main(){
    unsigned checks=0;
    {
        Fixture f;auto a=allocations.load();f.before();f.after();assert(allocations.load()==a);++checks;
        auto s=f.stats();assert(s.completedPairs==1&&s.comparedCalls==1&&s.comparedSamples==8&&s.bitDifferentSamples==0&&s.maximumAbsoluteDifference==0&&s.inputHash==s.outputHash&&s.nonzeroInputSamples==8&&s.finiteSamples==8&&s.inputSumSquares==3.75&&s.outputSumSquares==3.75);++checks;
    }
    {
        Fixture f;f.before();for(int c=0;c<2;++c)for(int n=0;n<4;++n)f.out[c][n]=f.in[c][n]*2;f.after();
        auto s=f.stats();assert(s.bitDifferentSamples==8&&s.maximumAbsoluteDifference==1.0&&s.differenceSumSquares==3.75&&s.inputHash!=s.outputHash);++checks;
    }
    {
        Fixture f;f.output.channelBuffers32=f.in;f.before();for(int c=0;c<2;++c)for(float& v:(c?f.right:f.left))v*=2;f.after();auto s=f.stats();assert(s.bitDifferentSamples==8&&s.differenceSumSquares==3.75);++checks;
    }
    {
        Fixture f;f.input.numChannels=f.output.numChannels=1;f.before();f.after();assert(f.stats().comparedSamples==4);++checks;
    }
    {
        Fixture f;f.data.numInputs=2;f.before();f.after();assert(f.stats().unsupportedCalls==1&&f.stats().comparedCalls==0);++checks;
    }
    {
        Fixture f;f.output.numChannels=1;f.before();f.after();assert(f.stats().unsupportedCalls==1);++checks;
    }
    {
        Fixture f;f.data.symbolicSampleSize=kSample64;f.before();f.after();assert(f.stats().unsupportedCalls==1);++checks;
    }
    {
        Fixture f;f.data.numSamples=8193;f.before();f.after();assert(f.stats().frameLimitSkips==1&&f.stats().comparedSamples==0);++checks;
    }
    {
        Fixture f;f.data.numSamples=-1;f.before();f.after();assert(f.stats().unsupportedCalls==1);++checks;
    }
    {
        Fixture f;f.input.channelBuffers32=f.output.channelBuffers32=nullptr;f.input.silenceFlags=f.output.silenceFlags=3;f.before();f.after();auto s=f.stats();assert(s.silentNullChannels==4&&s.comparedSamples==8&&s.bitDifferentSamples==0);++checks;
    }
    {
        Fixture f;f.in[0]=nullptr;f.before();f.after();assert(f.stats().unsupportedCalls==1);++checks;
    }
    {
        Fixture f;f.before();f.out[0]=nullptr;f.after();assert(f.stats().unsupportedCalls==1&&f.stats().comparedSamples==0);++checks;
    }
    {
        Fixture f;f.data.numSamples=0;f.input.channelBuffers32=f.output.channelBuffers32=nullptr;f.before();f.after();auto s=f.stats();assert(s.zeroFrameCalls==1&&s.comparedCalls==1&&s.comparedSamples==0);++checks;
    }
    {
        Fixture f;f.left[0]=std::numeric_limits<float>::quiet_NaN();f.before();f.outLeft[1]=std::numeric_limits<float>::infinity();f.after();assert(f.stats().nonfiniteSamples==2&&std::isfinite(f.stats().differenceSumSquares));++checks;
    }
    {
        Fixture f;f.before();f.after(kResultFalse);assert(f.stats().nativeFailures==1&&f.stats().comparedCalls==0);++checks;
    }
    {
        Fixture f;f.before();f.data.numSamples=3;f.after();assert(f.stats().unsupportedCalls==1&&f.stats().comparedCalls==0);++checks;
    }
    {
        Fixture f;f.before();f.before();f.after();f.after();auto s=f.stats();assert(s.overlappingBefore==1&&s.completedPairs==1&&s.comparedCalls==1);++checks;
    }
    {
        Fixture f;f.before();std::thread other([&f]{auto thread=f.thread;thread.nativeThreadToken=2;native_hooks::NativeProcessTelemetry t{native_hooks::NativeProcessTelemetryPhase::BeforeOriginal,f.record.actualProcessor.load(),&f.data,false,kResultOk};f.observer.observe(f.record,t,thread);t.phase=native_hooks::NativeProcessTelemetryPhase::AfterOriginal;t.originalInvoked=true;f.observer.observe(f.record,t,thread);});other.join();f.after();auto s=f.stats();assert(s.overlappingBefore==1&&s.rejectedAfter==1&&s.completedPairs==1);++checks;
    }
    {
        Fixture f;f.after();assert(f.stats().rejectedAfter==1);++checks;
    }
    {
        Fixture f;f.thread.nativeThreadToken=0;f.before();f.after();assert(f.stats().rejectedAfter==2&&f.stats().completedPairs==0);++checks;
    }
    {
        Fixture f;f.thread.insideNativeProcess=false;f.before();f.after();assert(f.stats().identityFailures==1&&f.stats().comparedCalls==0);++checks;
    }
    {
        Fixture f;f.before();auto original=f.record.actualProcessor.load();f.record.actualProcessor.store(reinterpret_cast<IAudioProcessor*>(std::uintptr_t{0x12350000}));f.after();auto s=f.stats();assert(s.unpairedAfter==1&&s.comparedCalls==0);f.record.actualProcessor.store(original);f.before();f.after();assert(f.stats().comparedCalls==1);++checks;
    }
    {
        Fixture f;f.left[0]=0.0f;f.before();f.outLeft[0]=-0.0f;f.after();assert(f.stats().bitDifferentSamples==1&&f.stats().differenceSumSquares==0);++checks;
    }
    {
        std::uint64_t n=UINT64_MAX-1;assert(process_observer::saturatingAdd(n,1)&&n==UINT64_MAX);assert(!process_observer::saturatingAdd(n,1)&&n==UINT64_MAX);n=1;assert(!process_observer::saturatingAdd(n,UINT64_MAX)&&n==UINT64_MAX);++checks;
    }
    {
        Fixture f;std::atomic<bool> done{false};std::thread reader([&]{while(!done.load()){process_observer::Snapshot s;if(f.observer.trySnapshot(s)){assert(s.comparedCalls==s.completedPairs);assert(s.comparedSamples==s.comparedCalls*8);assert(s.bitDifferentSamples==0);}}});
        for(int n=0;n!=10000;++n){f.before();f.after();}done=true;reader.join();assert(f.stats().completedPairs==10000);++checks;
    }
    {
        Fixture f;std::array<float,8192> a{},b{};for(std::size_t n=0;n<a.size();++n)a[n]=b[n]=static_cast<float>(n)/8192;
        f.input.numChannels=f.output.numChannels=1;f.in[0]=a.data();f.out[0]=b.data();f.data.numSamples=8192;
        auto before=allocations.load();f.before();f.after();assert(allocations.load()==before);auto s=f.stats();assert(s.comparedFrames==8192&&s.comparedSamples==8192&&s.nonzeroInputSamples==8191);++checks;
    }
    {
        Fixture f;f.input.silenceFlags=f.output.silenceFlags=3;f.before();f.outLeft[0]*=2;f.after();auto s=f.stats();assert(s.bitDifferentSamples==1&&s.silentFlagContradictions==16&&s.nonzeroInputSamples==8);++checks;
    }
    std::cout<<"PASS "<<checks<<" observer controls; native audio calls were synthetic, no DAW or plugin launched\n";
}
