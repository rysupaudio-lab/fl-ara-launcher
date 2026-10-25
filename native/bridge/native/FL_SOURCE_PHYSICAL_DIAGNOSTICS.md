# First physical-identity rejection diagnostics (C9)

This is observation only. Both `PhysicalIdentity` predicates and their rejection,
cache invalidation, output-preservation and stale-hold behavior remain unchanged.
It does not identify a voice as a preview or qualify audio output.

Each process-lifetime `Control` owns four one-shot slots. A writer tries at most
four strong compare/exchanges from Empty (0) to Writing (1), copies its already
observed `FLStandaloneSourceRead` plus protected-table metadata, then release
stores Ready (2) **before** the existing fault invalidation. Slots never reset,
recycle or overwrite, including after status reads, fault drains, cache updates
and table removal. There is no incrementing reservation index to wrap. When all
slots are Writing/Ready, a sticky atomic overflow flag is set and capture returns
quietly. No audio/native/SDK/file/UI calls, allocation, lock or wait is added.

The existing `ReaderGuard` protects every Batch/Source/Region used during the
copy. Captured model/manager/source metadata is prepared off RT in immutable
`Source` rows. Address fields are copied numeric values, never handles that a
later status reader dereferences. A retired table can be freed after its readers
drain; its diagnostic survives in the retained Control without retaining that
model or cache.

`FLStandaloneOverlayInstanceStats` and `FLStandaloneOverlayFaultRecord` expose:

- `physicalIdentityDiagnostics`: fixed array of four POD records;
- `physicalIdentityDiagnosticReadyMask`: bits 0..3 identify immutable ready slots;
- `physicalIdentityDiagnosticOverflow`: at least one capture found no free slot.

Read only slots with their ready bit set. Main-thread copies use acquire loads,
never wait for Writing and never clear readiness. Slot index remains stable even
if a later slot becomes Ready first. Diagnostic records are intentionally retained
when the ordinary coalesced fault mailbox is drained.

`branch` is `MappingAgreementA` (1) for missing/disagreeing entry, physical lookup
or indexed lookup; it is `ObservedPhysicalSourceB` (2) for the later voice/reader,
channel object, sample object or mixer comparison. `mismatchBits` names each
failed comparison using `FLStandalonePhysicalIdentityMismatch`. `presenceMask`
bits 0/1/2 mean entry/physical/indexed exists. `agreementMask` bits 0/1/2 mean that
candidate belongs to the selected owner batch; bits 3/4/5 mean source equality for
entry-physical, entry-indexed and physical-indexed respectively, with both present.

Each candidate carries its own captured model and native manager, pointer-array,
manager-member, native channel, source index, mixer, sample/PCM and class numeric
provenance. The entry also has its captured 88-byte region record. The observed
POD includes the original 88-byte record and native callback shape/positions.
It copies only metadata, never audio samples or additional native memory.

Rejected reads can contain nonfinite doubles before later format guards. JSON
serialization must guard all five observed doubles (`nativeRate`,
`nativeRateFactor`, `readerStep`, `sourceStartFrame`,
`preOriginalReaderPosition`), write null for nonfinite numeric values and retain
raw 64-bit representations separately when exact diagnosis is needed. Do not let
a malformed diagnostic prevent the rest of status from being written.

Offline controls exercise both branches, distinct indexed-source provenance,
nonfinite POD values, four-slot saturation, more than 1024 overflow attempts,
near-wrap unrelated telemetry counters, fault drains/republication, concurrent
writers/acquire snapshots, and retired-table/model destruction. They are synthetic
metadata controls; normal FL C9 runtime and audio output require separate proof.
