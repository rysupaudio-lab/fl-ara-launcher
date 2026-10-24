#include "FLNativeProcessObserver.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace rysup::fl_ara::process_observer {
namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;
constexpr std::uint64_t fnvOffset=14695981039346656037ULL, fnvPrime=1099511628211ULL;
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::uint64_t doubleBits(double v) noexcept { std::uint64_t u;std::memcpy(&u,&v,8);return u; }
double fromBits(std::uint64_t v) noexcept { double d;std::memcpy(&d,&v,8);return d; }
std::uint32_t floatBits(float v) noexcept { std::uint32_t u;std::memcpy(&u,&v,4);return u; }
void hash(std::uint64_t& h,std::uint32_t u) noexcept {
    for(unsigned n=0;n!=4;++n){h^=(u>>(n*8))&255U;h*=fnvPrime;}
}
void atomicSaturatingIncrement(std::atomic<std::uint64_t>& target) noexcept {
    // One bounded CAS attempt; contention is itself diagnostic, never a reason
    // to spin on the audio thread. A missed increment makes this a lower bound.
    auto old=target.load(std::memory_order_relaxed);
    if(old!=UINT64_MAX)target.compare_exchange_strong(old,old+1,std::memory_order_relaxed);
}
bool shape(const ProcessData& d) noexcept {
    return d.symbolicSampleSize==kSample32 && d.numSamples>=0 &&
        d.numInputs==1 && d.numOutputs==1 && d.inputs && d.outputs &&
        d.inputs[0].numChannels>=1 && d.inputs[0].numChannels<=2 &&
        d.outputs[0].numChannels==d.inputs[0].numChannels;
}
bool channelReadable(const AudioBusBuffers& bus,int c,int n) noexcept {
    if(n==0)return true;
    // A silent null channel can be represented numerically without touching
    // its buffers. This is observation only, not SDK bus-activation validation.
    return (bus.silenceFlags&(std::uint64_t{1}<<c)) ||
        (bus.channelBuffers32 && bus.channelBuffers32[c]);
}
float value(const AudioBusBuffers& bus,int c,int f) noexcept {
    // Observe the actual raw sample whenever supplied, even if a stale silence
    // flag disagrees. Only an absent, explicitly silent channel synthesizes 0.
    return (bus.channelBuffers32 && bus.channelBuffers32[c]) ? bus.channelBuffers32[c][f] : 0.0f;
}
}
bool saturatingAdd(std::uint64_t& value,std::uint64_t increment) noexcept {
    if(increment>UINT64_MAX-value){value=UINT64_MAX;return false;}
    value+=increment;return true;
}
NativeProcessObserver::NativeProcessObserver(native_hooks::InstanceRecord& r) noexcept
    :boundRecord(&r),boundIdentity(r.identity) {
    totals.inputHash=totals.outputHash=fnvOffset;publish();
}
void NativeProcessObserver::add(std::uint64_t& v,std::uint64_t n) noexcept {
    if(!saturatingAdd(v,n) && &v!=&totals.counterOverflows)
        saturatingAdd(totals.counterOverflows,1);
}
void NativeProcessObserver::publish() noexcept {
    const std::uint64_t values[fieldCount]={totals.completedPairs,totals.comparedCalls,totals.nativeFailures,
        totals.unsupportedCalls,totals.identityFailures,totals.unpairedAfter,totals.frameLimitSkips,
        totals.zeroFrameCalls,totals.nonfiniteSamples,totals.silentNullChannels,totals.comparedSamples,
        totals.bitDifferentSamples,totals.comparedFrames,totals.inputHash,totals.outputHash,
        totals.counterOverflows,doubleBits(totals.differenceSumSquares),
        doubleBits(totals.maximumAbsoluteDifference),totals.nonzeroInputSamples,
        totals.finiteSamples,doubleBits(totals.inputSumSquares),doubleBits(totals.outputSumSquares),
        totals.silentFlagContradictions};
    sequence.fetch_add(1,std::memory_order_acq_rel);
    for(unsigned i=0;i!=fieldCount;++i)published[i].store(values[i],std::memory_order_relaxed);
    sequence.fetch_add(1,std::memory_order_release);
}
void NativeProcessObserver::finish() noexcept {
    dataAddress=processorAddress=0;supported=false;frameLimited=false;
    frames=channels=0;publish();activeThread.store(0,std::memory_order_release);
}
void NativeProcessObserver::observe(native_hooks::InstanceRecord& record,
    const native_hooks::NativeProcessTelemetry& t,const native_hooks::ThreadInfo& thread) noexcept {
    using native_hooks::NativeProcessTelemetryPhase;
    const auto token=thread.nativeThreadToken;
    if(t.phase==NativeProcessTelemetryPhase::BeforeOriginal){
        if(!token){atomicSaturatingIncrement(rejected);return;}
        std::uintptr_t expected=0;
        if(!activeThread.compare_exchange_strong(expected,token,std::memory_order_acq_rel)){
            atomicSaturatingIncrement(overlaps);
            if(expected==token && nestedDepth!=std::numeric_limits<unsigned>::max())++nestedDepth;
            return;
        }
        nestedDepth=0;supported=false;frameLimited=false;
        dataAddress=reinterpret_cast<std::uintptr_t>(t.actualProcessData);
        processorAddress=reinterpret_cast<std::uintptr_t>(t.actualNativeProcessor);
        if(&record!=boundRecord || record.identity!=boundIdentity || !thread.insideNativeProcess ||
           !t.actualNativeProcessor || record.actualProcessor.load(std::memory_order_relaxed)!=t.actualNativeProcessor ||
           !t.actualProcessData || t.originalInvoked){add(totals.identityFailures);return;}
        const auto& d=*t.actualProcessData;
        frameLimited=d.numSamples>static_cast<int>(maximumFrames);
        if(!shape(d)||frameLimited)return;
        frames=d.numSamples;channels=d.inputs[0].numChannels;inputSilenceFlags=d.inputs[0].silenceFlags;
        for(int c=0;c!=channels;++c)if(!channelReadable(d.inputs[0],c,frames))return;
        for(int c=0;c!=channels;++c){
            if(frames && (d.inputs[0].silenceFlags&(std::uint64_t{1}<<c)) &&
               (!d.inputs[0].channelBuffers32 || !d.inputs[0].channelBuffers32[c]))add(totals.silentNullChannels);
            for(int f=0;f!=frames;++f)input[c*maximumFrames+f]=value(d.inputs[0],c,f);
        }
        supported=true;return;
    }
    if(!token || activeThread.load(std::memory_order_acquire)!=token){atomicSaturatingIncrement(rejected);return;}
    if(nestedDepth){--nestedDepth;return;}
    if(dataAddress!=reinterpret_cast<std::uintptr_t>(t.actualProcessData) ||
       processorAddress!=reinterpret_cast<std::uintptr_t>(t.actualNativeProcessor)){
        add(totals.unpairedAfter);finish();return;
    }
    add(totals.completedPairs);
    if(!t.originalInvoked || t.originalResult!=kResultOk){add(totals.nativeFailures);finish();return;}
    if(!supported){add(totals.unsupportedCalls);if(frameLimited)add(totals.frameLimitSkips);finish();return;}
    const auto& d=*t.actualProcessData;
    if(&record!=boundRecord || record.identity!=boundIdentity || !thread.insideNativeProcess ||
       !shape(d) || d.numSamples!=frames || d.inputs[0].numChannels!=channels){
        add(totals.unsupportedCalls);finish();return;
    }
    for(int c=0;c!=channels;++c)if(!channelReadable(d.outputs[0],c,frames)){
        add(totals.unsupportedCalls);finish();return;
    }
    add(totals.comparedCalls);add(totals.comparedFrames,frames);
    if(!frames)add(totals.zeroFrameCalls);
    for(int c=0;c!=channels;++c){
        if(frames && (d.outputs[0].silenceFlags&(std::uint64_t{1}<<c)) &&
           (!d.outputs[0].channelBuffers32 || !d.outputs[0].channelBuffers32[c]))add(totals.silentNullChannels);
        for(int f=0;f!=frames;++f){
            const float a=input[c*maximumFrames+f],b=value(d.outputs[0],c,f);
            if((inputSilenceFlags&(std::uint64_t{1}<<c))&&a!=0.0f)add(totals.silentFlagContradictions);
            if((d.outputs[0].silenceFlags&(std::uint64_t{1}<<c))&&b!=0.0f)add(totals.silentFlagContradictions);
            const auto ai=floatBits(a),bi=floatBits(b);
            hash(totals.inputHash,ai);hash(totals.outputHash,bi);
            add(totals.comparedSamples);if(ai!=bi)add(totals.bitDifferentSamples);
            if(!std::isfinite(a)||!std::isfinite(b)){add(totals.nonfiniteSamples);continue;}
            add(totals.finiteSamples);if(a!=0.0f)add(totals.nonzeroInputSamples);
            totals.inputSumSquares+=static_cast<double>(a)*a;
            totals.outputSumSquares+=static_cast<double>(b)*b;
            const double difference=static_cast<double>(b)-static_cast<double>(a);
            const double absolute=std::fabs(difference);
            totals.differenceSumSquares+=difference*difference;
            if(absolute>totals.maximumAbsoluteDifference)totals.maximumAbsoluteDifference=absolute;
        }
    }
    finish();
}
bool NativeProcessObserver::trySnapshot(Snapshot& out) const noexcept {
    for(unsigned attempt=0;attempt!=8;++attempt){
        const auto before=sequence.load(std::memory_order_acquire);if(before&1)continue;
        std::uint64_t v[fieldCount];
        for(unsigned i=0;i!=fieldCount;++i)v[i]=published[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(sequence.load(std::memory_order_acquire)!=before)continue;
        Snapshot s;
        s.completedPairs=v[0];s.comparedCalls=v[1];s.nativeFailures=v[2];s.unsupportedCalls=v[3];
        s.identityFailures=v[4];s.unpairedAfter=v[5];s.frameLimitSkips=v[6];s.zeroFrameCalls=v[7];
        s.nonfiniteSamples=v[8];s.silentNullChannels=v[9];s.comparedSamples=v[10];
        s.bitDifferentSamples=v[11];s.comparedFrames=v[12];s.inputHash=v[13];s.outputHash=v[14];
        s.counterOverflows=v[15];s.differenceSumSquares=fromBits(v[16]);s.maximumAbsoluteDifference=fromBits(v[17]);
        s.nonzeroInputSamples=v[18];s.finiteSamples=v[19];s.inputSumSquares=fromBits(v[20]);s.outputSumSquares=fromBits(v[21]);
        s.silentFlagContradictions=v[22];
        s.overlappingBefore=overlaps.load(std::memory_order_relaxed);s.rejectedAfter=rejected.load(std::memory_order_relaxed);
        out=s;return true;
    }
    return false;
}
} // namespace rysup::fl_ara::process_observer
