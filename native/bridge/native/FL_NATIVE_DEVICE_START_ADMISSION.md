# Owned QA device-start admission

This extends explicit QA mode only. Normal-mode interposers forward the actual
original public API, once, with unchanged arguments. No vendor/Apple code,
device-global property, user install, or process memory instruction is patched.
The runtime result remains unqualified until actual owned FL runtime checks.

The actual Apple CoreAudio component profile is:

- /System/Library/Components/CoreAudio.component/Contents/MacOS/CoreAudio
- universal SHA256 64635af4212b6c5f257182cdbcbcdf553081a9e23f971fc4b9e0f4b1fb901f87
- ARM64e slice SHA256 10fb189b357f421c17e3273730bbed16c4d5361ef2c049569482173e42178382

Actual return-PC/callback pairs, relative to that exact admitted image:

| Pair | CreateIOProcID caller | Native callback | DeviceStart caller | StartAtTime caller |
| --- | --- | --- | --- | --- |
| AUHAL (1) | 0x38bcc | 0xe5cb0 | 0x391c4 | refused |
| Generic (2) | 0x81b8 | 0x9fd8 | 0x9bec | 0x9bdc |

Those are return addresses after the original call instructions. Provenance
requires the exact admitted image path and full-file hash as well as the pair.
Apple signs ARM64e callback pointers. FLPointerProvenance.h uses the public
SDK's ptrauth_strip intrinsic for comparisons only. NativeApplication.mm must
compile with -fptrauth-intrinsics; -fptrauth-calls is not enabled and the
calling ABI is unchanged. Original typed signed pointers are forwarded intact
and are never replaced with the stripped comparison pointer.

FLUnitDeviceGate is SDK-free and stores bounded native identities, unit
generations and IOProc associations. CurrentDevice/Initialize public-API scopes
capture the actual original CreateIOProcID result for that same unit/device,
with exact Apple creation/callback provenance. Earlier capture survives later
render-callback registration; the device lifetime and Sink preparation are
separate generations. Failed creates/starts, actual unit lifecycle changes and
destroy attempts revoke authority. A late success cannot revive it.

Before the original AudioOutputUnitStart, the Sink freshly prepares and captures
an exact unit/device/record/generation ticket, then acquires a nonblocking start
lease. The lease stays held through the original API, every nested DeviceStart
decision and closing the same-thread gate scope. Competing callback removal,
critical setters, uninitialize and disposal cannot execute their originals while
that lease is held. Render callbacks never acquire or wait on this lease.
Per-unit lifecycle reservations exist even before the first Sink record, so an
untracked first mutation cannot race initial registration/start. Registration
and preparation use the record from the acquired reservation rather than a
stale pre-acquire lookup.

A nested start needs the current admitted process, exact test-device UID and
virtual transport, same-thread original OutputStart scope, still-held Sink
ticket, live captured IOProc for that exact unit/device/generation, and paired
Apple start caller. Direct, unrelated, cross-thread, null, stale or mismatched
starts return the ordinary error. There is no blanket same-device or TLS permit.
Restriction-only TLS suppresses inheritance during unrelated public lifecycle
calls. No foreign API or validator runs under the Gate's table claim.

Destroy first reserves the exact IOProc identity, including an unknown one.
The owner then acquires the known unit's Sink mutation lease, or verifies the
same-thread already-held exact-unit mutation for nested teardown. It commits
revocation before calling the original Destroy once and holds both logical
leases through return. A busy start abandons destruction without calling the
original or revoking the live binding. Reservation prevents concurrent capture,
start and identity reuse. Failed destroy/dispose cannot revive old authority.

New numeric NativeAudioTestStats fields expose profile admission, start/
AtTime/create/destroy entries, original calls/failures, explicit start and
lifecycle refusals, suppressed captures, Sink lifecycle refusals, Gate counters
and last reason/stage/device, and lastDeviceIO. No IOProc identity, callback
absolute address, path, or device UID string is exposed by these diagnostics.

lastDeviceIO.api: 1 Start, 2 StartAtTime, 3 CreateIOProcID, 4 DestroyIOProcID.
For starts, stage1 means app/profile refusal, 2 restriction-only suppression,
3 exact-device verification, 4 Gate decision, 5 original returned. For destroy,
6 reservation refusal, 7 owning-unit lease refusal, 8 commit refusal, 5 original
returned. Create stage5 reports the actual original result and capture flag.
Caller/callback source: 0 unresolved, 1 exact Apple profile, 2 other image.
Offsets are image-relative. lastOutputStart retains its earlier stages and
adds 7 prepared-ticket/lease refusal, 8 Gate-scope refusal, 9 Apple profile
unsupported, 10 device changed during admission.

The synthetic controls exercise Sink/IOProc policy interaction, not a DAW,
plugin or audio device. They cover held-start versus destroy/mutation,
first-mutation versus first registration, stale preparation, cross-thread
scopes, failure/reuse/revocation, bounded exhaustion and original-call counts.
No real Initialize, Start, Render, IOProc creation or driver IO occurs in these
controls. Actual runtime qualification remains separate.
