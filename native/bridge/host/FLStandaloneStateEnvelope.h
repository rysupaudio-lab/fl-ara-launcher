#pragma once
// Standalone FL owner only. Includes the NEW owner-neutral host types; never
// include the old AU integration's GenericARAHost/NativeStateEnvelope headers.
#include "StandaloneARACoordinator.h"
#include <pluginterfaces/base/ibstream.h>
#include <optional>

namespace rysup::ara_lab {
inline constexpr std::size_t kFLStandaloneEnvelopeMaximumBytes=256u*1024u*1024u;
struct FLStandaloneStateEnvelope {
    StandaloneSavedEditorArchive archive;
    Bytes nativeComponentState;
};
// Wire: RYSFLAPP(8), LEversion32=1, LEpayloadLength64, SHA256(payload)(32),
// binary CFPropertyList. Configured graph only; opaque ARA/native bytes are exact.
// Genuine last-clip deletion may retain source/modification objects with no
// tracks/regions, or leave the insert entirely empty. Empty native records are
// admitted only with exact empty-model digests and actual ready archive capture.
// Serializes graph, stable native authority, clock format/6 native clock bytes,
// and exact ordered raw88 records plus native-verified audio-row indices. No runtime identity, PID, pointers, source
// audio copies (opaque native state is preserved), resolver, transport position or process-local project identity.
Bytes encodeFLStandaloneStateEnvelope(const FLStandaloneStateEnvelope&);
std::optional<FLStandaloneStateEnvelope> decodeFLStandaloneStateEnvelope(const Bytes&);
bool hasFLStandaloneStateEnvelopeMagic(const Bytes&)noexcept;
void validateFLStandaloneStateEnvelope(const FLStandaloneStateEnvelope&);

struct FLStandaloneRestoreGraphValidation {
    StandaloneSnapshotData current;
    bool objectIDRelocationVerified=false;
    StandaloneNativeModelAuthority verifiedOldIncomingNativeModel;
    ArchiveObjectIDRelocation objectIDs;
};
// Pure metadata/immutable-source validation; does not call a plug-in, native FL,
// ARA model or file API. Reads bounded providers from CURRENT verified copied
// native RAM to verify signed-int16 source PCM hashes. All non-ID raw88 bytes
// and their order must match. Only explicitly classified audio clipID +0x20 (4 bytes) may regenerate;
// source/modification IDs stay exact and region IDs map through paired records.
// Root MUST separately attest the actual native child setState stream, sealed
// caller, owner/load phase and coordinator hold. This is not that authority.
FLStandaloneRestoreGraphValidation validateFLStandaloneRestoreGraph(
    const StandaloneSavedEditorArchive&,const StandaloneSnapshotData&);

// Unique helper namespace permits old/new transport objects in one build
// without confusing the two magic formats or including old host definitions.
namespace fl_app {
class MemoryIBStream final:public Steinberg::IBStream,public Steinberg::ISizeableStream {
public:
    explicit MemoryIBStream(Bytes = {});
    const Bytes& bytes()const noexcept{return data_;}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID,void**)override;
    Steinberg::uint32 PLUGIN_API addRef()override;
    Steinberg::uint32 PLUGIN_API release()override;
    Steinberg::tresult PLUGIN_API read(void*,Steinberg::int32,Steinberg::int32*)override;
    Steinberg::tresult PLUGIN_API write(void*,Steinberg::int32,Steinberg::int32*)override;
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64,Steinberg::int32,Steinberg::int64*)override;
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64*)override;
    Steinberg::tresult PLUGIN_API getStreamSize(Steinberg::int64&)override;
    Steinberg::tresult PLUGIN_API setStreamSize(Steinberg::int64)override;
private:
    std::atomic<Steinberg::uint32> refs_{1};Bytes data_;Steinberg::int64 position_=0;
    ~MemoryIBStream()=default;
};
Bytes readRemaining(Steinberg::IBStream&,std::size_t maximum=kFLStandaloneEnvelopeMaximumBytes);
void writeAll(Steinberg::IBStream&,const Bytes&);
// Always restores position; unsupported tell/seek or read failure throws.
// False means forward the ORIGINAL stream unchanged. True mandates strict
// decode; recognized corrupt input never becomes ordinary/native passthrough.
bool peekMagicAndRestorePosition(Steinberg::IBStream&);
} // namespace fl_app
} // namespace rysup::ara_lab
