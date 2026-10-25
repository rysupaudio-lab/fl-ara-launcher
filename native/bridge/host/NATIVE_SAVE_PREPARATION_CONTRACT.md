# Genuine ARA archive preparation for native Save

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


Source contract only. The normal-app/native Save transaction still requires actual
runtime verification. No native getState event or original-state call is fabricated.

## Prepare outside native Save

After actual source-cache publication at the owned stopped, drained, worker-free
model boundary:

```cpp
auto prepared = coordinator.prepareEditorArchiveForNativeSave(instanceID, snapshot);
```

The snapshot is a genuine coordinator-minted current native RAM/direct-clock token.
The current applied model, successful cache snapshot, authorized cache ticket and
actual ARA edit revision must match. A no-regions/true-empty model needs its real
authorized `noSourceOverlayRequired` cache batch; it renders no DSP but is still a
current readiness generation. Retained unreferenced source/modification objects
remain in the document and its actual archive.

Preparation calls the genuine `HostController::captureArchiveOnly` through the
normal stopped model API. It does not call ordinary component getState, adjust
original-state counters, pretend a native serializer is active, or copy ordinary
component bytes. Real archive capture may flush pending ARA notifications. The
same cache ticket and edit revision are checked afterward; a change produces
`SourceCacheNotReady` and no prepared archive.

`PreparedEditorArchive` is immutable and privately constructed. Copies retain the
same actual preparation authority. Its public `identity()` exposes actual native
identity/seals, capture serial, cache ticket, edit revision and no-overlay status.
It deliberately has no unvalidated archive/blob getter.

## File-open preflight without ARA calls

Before the real native saver opens/creates/truncates the destination, the owner
captures actual current native RAM/clock under its proven native mix boundary,
without ARA/model/archive/plug-in work, and calls:

```cpp
StandaloneSavedEditorArchive frozen =
    coordinator.validatePreparedEditorArchiveForNativeSave(
        instanceID, prepared, actualCurrentNativeRAM);
```

This path is pure bounded metadata and atomic observation. It does not call the
owner adapter, source resolver/provider, worker probe, native engine or ARA SDK.
Metadata-only current data may omit its resolver. The current direct-RAM/clock,
association and no-file-fallback proof flags still must be genuine and valid.

It requires the same coordinator/process/instance/component/native owner/insert,
module/engine/decoder seals, native project generation/version, exact source/edit/
region physical identities, source descriptors and complete physical Graph,
native raw record bytes and digest, audio-row classification, PCM digest and
actual stopped native clock. A changed descriptor or raw byte fails even if its
claimed aggregate hash is copied unchanged. Ordering of equivalent metadata
vectors is irrelevant; object identity and content are not.

The exact prepared cache epoch must still be the coordinator's current published
epoch and remain authorized. The actual edit revision must still equal both the
prepared and published revision. These checks run again after copying the archive
for the native transaction. Current full-source cache provenance may ignore the
stopped playhead position because its PCM is source-coordinate data; the prepared
archive/current Save snapshot comparison itself requires the actual position too.

Any stale, dirty, pending, wrong-instance or mismatched model refuses validation.
The validator does not silently capture a new archive, invalidate/repair metadata,
or call SDK under the native Save outer lock.

## Actual native serializer and transaction responsibility

The owner freezes the returned genuine archive per insert in a Save transaction
before file creation. Later actual native getState calls the ordinary original
once and encodes those actual ordinary bytes with the frozen archive. There are
no additional ARA calls under the native Save outer lock. Keep current-ticket
checks through the transaction so a late actual edit prevents native commit.

Playing, unavailable or not-ready Save must be refused as a **whole native Save
before destination creation/truncation**. Refusing only a component chunk is not
sufficient: the proven native saver can drop that chunk and continue. The owner
also guards native commit/replace if a later component/codec/transaction step
fails. These file-open and final commit guarantees are native bridge responsibilities;
the coordinator API alone does not claim that native FL honored Save refusal.

The tests exercise the exact pure metadata comparator, immutable preparation API
shape and actual atomic cache-gate implementation without constructing a factory,
fabricating an ARA archive, loading a plug-in or launching an application. Real
prepared archive capture and actual native whole-Save veto/commit still need the
owning native runtime tests.
