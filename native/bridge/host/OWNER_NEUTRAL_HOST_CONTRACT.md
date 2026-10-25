# Standalone native ARA host integration

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


This is the new normal-application source line. The historical FL AU laboratory
host is unchanged. Compile only the new copies of `GenericARAHost.cpp`,
`VST3ARAEntry.cpp`, `OwnedVST3PlaybackCompanion.cpp` and
`StandaloneARACoordinator.cpp`, plus the real SDK host dispatch/debug sources.
Do not also link the historical host/coordinator: the common host types intentionally
keep their namespace and would violate the one-definition rule.

The generic layer has no AudioUnit, CFPropertyList, app launcher, audio device,
file audio importer, fixed plug-in product/CID or private QA interface. The owner
adapter is a trusted native integration boundary; this is not a security sandbox.
Current native RAM/clock/physical-owner proofs come from the actual sealed native
decoder and owner hooks. The host does not turn a saved file or serializing getter
into a claim about current native RAM.

## Binding and native lifetime

Call `bindAfterOriginalInitialize` only from the actual fresh native component's
successful original initialize hook, before its first state/activation/processing,
process-context query or editor creation. Every authority flag defaults false.
Supply the exact native IPluginFactory, actual audio/ARA class IDs, independently
retained already-loaded module lifetime and actual component/engine/insert identity.
The coordinator creates an independent matching `ARA::IMainFactory` through that
same native factory. Genuine ARA factory identity and SDK class pairing are checked;
vendor interfaces may live in a separately sealed shared framework.

The normal bridge also supplies the optional last-moment actual borrowed-native
freshness callback. It rereads real hook counters/owner references immediately
before entry2 bind, after factory/document creation, without SDK/native calls.
The narrow initial empty-document foreign-lock source review is documented in
`INITIAL_EMPTY_BIND_LOCK_REVIEW.md`; it does not qualify later model/cache work
under a foreign lock or prove vendor worker behavior.

The borrowed native component binds once, EditorRenderer plus EditorView only.
Its original state, activation, processing, editor and termination remain native
owner operations. Observe every original lifecycle/state result. The coordinator
never calls borrowed native component state recursively. It retains the factory,
independent ARA provider and module separately from that component.

Before original native termination, close its editor, actually stop processing,
deactivate and drain, then call `prepareForOriginalTerminate` from the exact native
component's synchronous pre-original-terminate callback on its model thread. Its
distinct `verifyActualNativeTeardownBoundary` authority verifies the known component,
observed original setActive(false)/setProcessing(false), closed own process gate,
zero original native process calls in flight and original terminate not yet called.
The internal observed inactive flags and `HostController::safeForOwnedTeardown`
remain mandatory. This does **not** require the whole song to stop, current native
RAM/clock, or the global native engine pointer to remain installed: it deletes only
owned ARA models/interfaces around that already inactive component. The adapter's
default teardown verifier returns a closed result. Only after the actual
original termination succeeds should `observeOriginalTerminateResult` release the
last own factory/module references. An incomplete or uncertain teardown retains
those references rather than deleting a live foreign-owned component.

## Actual stopped native boundary and current authority

`StandaloneOwnerAdapter::verifyActualNativeHold` must verify the actual native
engine/insert owner, real stopped transport, closed/drained native process gate,
current model thread, and that no original child render/state/lifecycle call is
currently executing. A GUI pause indicator or fabricated inactive flag is insufficient.
The record's active/processing observations are never overwritten to manufacture
inactivity. Editor-only model work may run while the component remains active,
provided the real native owner is held and its callbacks are drained.

For standalone FL, the owner takes its proven native mix critical section only
for the bounded RAM/direct-clock copy, then releases it **before any ARA model,
plug-in SDK, archive or owned playback call**. Its process gate remains closed
through those operations. The generic coordinator does not acquire FL locks.

`captureActualCurrentSnapshot` invokes the adapter, rechecks the real hold and
validates the returned `StandaloneSnapshotData` before minting a private capture
token. The data contains current native component/owner/insert identity, immutable
RAM source providers, physical source/modification/region identities, exact copied
native records, engine/decoder/record/source/Graph seals and a twice-read actual
native clock. The FL clock bytes are the actual six PPQ/milliBPM bytes; sample
position/playing state are separate actual observations. No default tempo/PPQ is
provided. The current profile is 48 kHz and copied mono/stereo sources.

`nativeProjectIdentity` identifies the current actual native project generation
for live continuity; it is not merely the current save path. A native project
replacement must use its explicit load authority. It cannot masquerade as an
ordinary graph refresh. Physical identities and immutable PCM hashes determine
whether an existing model object may be retained.

## Same-document graph refresh

Pass an actual captured token to `refreshGraph`. Existing source IDs must retain
physical identity, PCM fingerprint and format/extent. Existing modification IDs
must retain native edit identity and source ownership. Existing region IDs must
retain native region identity and modification ownership. A real physical change
needs a new ID or an explicit native project replacement.

`reconcileEditorGraphWhileActive` updates the existing document controller:

- Existing source/modification SDK objects remain alive with their plug-in edits.
- Existing regions update position, trim, duration, track and names.
- New/removed sources, tracks, modifications and regions use real SDK create/destroy
  operations in dependency order. Deleted tracks remain until retained regions
  have moved away from them.
- Explicit verified native clone evidence calls actual `cloneAudioModification`
  before the old modification is removed. It requires the same immutable ARA
  source. No clone is guessed from equal PCM or geometry.

The standalone FL decoder uses one modification per actual native audio channel
and immutable source ID. Playlist duplicates/moves/trims therefore keep the same
modification naturally. New independent native channel/source identity gets its
own modification. Existing clips do not acquire fresh edit objects merely because
their Playlist geometry changes.

An authoritative snapshot may contain no regions/tracks while retaining real
source/modification objects, or may be a genuinely empty native model. Both are
valid. Removing the last Playlist clip removes only its region; retained channel
modification edits survive for Undo and actual archive capture. Cache planning
renders only modifications referenced by actual current regions. A no-regions
batch has `noSourceOverlayRequired=true`, no PCM caches, preserved unreferenced
modification IDs, and a newly authorized epoch ticket. The owner must clear the
old overlay or publish an explicitly empty immutable overlay table before
resuming native playback. No silent PCM or phantom ARA models are fabricated.

## Immediate cache invalidation and rebuild

Every actual ARA source/modification/region/document notification increments the
host edit revision and invokes the construction-time noexcept observer. The
observer first invalidates `StandaloneCacheGate` atomically, then closes the real
native overlay and coalesces model-thread rebuild work. It never calls ARA/native
SDK, waits or renders in that callback. Native Playlist/source/project change hooks
must immediately call `invalidateSourceCachesAndRequestRebuild` as well.

Only actual plug-in ARA notifications carry a nonzero edit revision. The observer
also forwards those revisions through the default-no-op
`queueActualNativeDocumentDirtyFromARA(instanceID, actualEditRevision)` owner
callback. Its atomic high-water relay suppresses duplicate/out-of-order older
revisions. Manual cache/host-graph invalidations use zero and do not queue native
document dirty state or increment the actual ARA revision. The owner callback
only queues atomics; its main-thread dispatcher calls the real public native
document-dirty API later. No fake parameter edit or native SDK call is made in
an ARA notification thread. A verified successful native commit advances the
owner's committed revision; newer revisions stay dirty and failed saves requeue.

At the next real stopped/drained boundary, capture/refresh current native data,
poll ARA model updates and call `rebuildWholeSourceCaches`. Each actual modification
uses an additional OWN Playback-only component from the same native factory and
the **same existing document controller**. The original native component is neither
adopted nor rendered by this helper. Internal full-source regions and own components
are removed/disposed before success. Exact model/edit epoch checks prevent stale
publication; a change during rendering refuses the batch.

Root must call `batch.gate->authorizes(batch.ticket)` at **every actual native
source-overlay read**, not only when installing a cache. Old immutable PCM may stay
allocated for readers, but its old ticket becomes unauthorized immediately. Keep
the real native gate held until a new valid batch is installed or ordinary native
audio fallback is selected. Do not resume user playback with an old cache.

The owner may supply the actual native component's original-only ordinary state
in `WholeSourceCacheOptions::ordinaryCompanionStateBeforeActivation`, with the
explicit apply flag true. Capture it through the original native getState once,
outside ARA/model/cache calls, under the owner's original-only TLS scope. Never
pass the bridge envelope or substitute an ARA/source archive. The additional child
is genuinely bound first, then receives those actual ordinary bytes once through
a bounded read-only IBStream/ISizeableStream, before its first activation. This
preserves real component-local parameters; the borrowed component is unchanged.

Advertised content types are optional. The helper polls actual incomplete flags
and records actual availability, without inventing notes or requiring every
advertised type to be available. A factory advertising zero types is supported.
Actual plug-in UI/parameter behavior and real DSP output still need runtime proof.
An actual SDK-reported incomplete analysis, or safely cleaned-up model revision/
publication-epoch race, throws the typed `SourceCacheNotReady`. The owner keeps
the gate closed and retries after polling at its next stopped tick. Contract,
unsupported timing/format and uncertain lifecycle/cleanup errors retain their
ordinary failure path; no string matching converts them to retryable analysis.

The current fixed-source cache deliberately refuses measured nonzero VST3 latency
or ARA region head/tail. Measurements occur after actual parameter-state restore
and processing setup. There is no fitted delay/gain, assumed zero latency or
invented preroll alignment. Native tails outside original source extent and
arbitrary channel layouts are not qualified. Third-party compatibility must be
reported against actual measured settings, not inferred from factory metadata.

## Native saves and project replacement

`captureArchiveOnly` requires the actual applied current RAM/clock/Graph token.
It captures the actual ARA archive and attaches current native authority, exact
copied native model records and actual verified audio-record indices. That sorted,
unique index list comes from native channel-kind classification, never raw record
bits. The owner serializer stores these extensions inside its bounded native state
envelope while calling original component state exactly once separately.

For an actual native load, the validator supplies current RAM/direct clock plus
the verified current native load call or attested deferred window for the exact
engine owner/component/insert. Its accepted load argument must be unconsumed and
the native event identity may be applied once. These are owner-neutral native
proofs; no fake AU setter/initializer exports or synthetic ClassInfo capture exist.

Two explicit archive modes exist: fresh before first original state, or an existing
bound native project replacement at a real stopped/drained boundary. The latter
destroys old source/modification objects, recreates current objects on the **same
bound document controller**, then restores the real archive. This clears previous
project edits instead of leaking them into a new native project.

An actual temporary native preflight lock/boundary refusal uses the public typed
`NativeModelBoundaryUnavailable`. Archive restoration rethrows it with the source
gate still closed and without permanently disabling the insert **only before**
the host's real replace/restore operation starts. The owner retains the fully
decoded native envelope/original bytes and retries at a later valid boundary.
The same exception after mutation starts remains a hard insert failure; it is
never a reason to continue a partially changed ARA document.

Identity restore remains exact. A specific verified native ID regeneration may
use complete bijective old/current source/modification/region mappings. The owner
must independently validate exact old incoming native provenance; current engine,
version, native clock and PCM remain exact, and audio/non-audio classifications
must match. The actual SDK ARARestoreObjectsFilter maps old source/modification IDs
to current objects; no opaque archive rewrite is performed. The next save captures
the coherent current IDs. A deliberate plain new native project uses
`applyPlainNativeProjectReplacement`; it does not silently drop a failed archive.

State/graph/archive failures close and visibly disable ARA for the affected insert,
preserving the native application/session and ordinary native audio path. The new
host's scoped ARA assertion handler reports failure/invalidation instead of
intentionally calling `std::terminate`. Uncertain native ownership/lifecycle is
never treated as successful cleanup.

## Validation status

The new source line passes strict C++17 compilation, public API compiler controls
and a small pure atomic cache-gate executable (including cross-thread invalidation).
That executable loads no plug-in or application and uses no audio hardware.
Same-DC reconciliation, normal FL loads/editor interaction, ordinary parameter-state
synchronization, third-party DSP and native application packaging still require
actual runtime verification. Historical standalone/FL AU laboratory qualification
does not automatically qualify this new integration source line.
