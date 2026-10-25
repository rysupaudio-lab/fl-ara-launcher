# SDK-free unit/device start policy

This is a bounded metadata gate, not an audio implementation. It creates, starts,
stops and destroys nothing. All identities are opaque integers. the native owner's native
wrappers must bind Pair to the measured, hash-verified Apple image and exact
callback/create/start instruction offsets. Passing an enum alone supplies no
provenance proof.

For the measured profile, AUHAL pair 1 is create caller 0x38bcc, callback 0xe5cb0,
Start caller 0x391c4. Generic pair 2 is create caller 0x81b8, callback 0x9fd8,
Start caller 0x9bec or StartAtTime caller 0x9bdc. Only Generic admits AtTime in
this policy. Offsets are verified by the native owner, never executed here.

The native owner begins a move-only CurrentDevice or Initialize capture scope
before its original call. Only actual successful nonnull CreateIOProcID output
can be recorded, in the current same-thread exact unit/device scope. Capture
can precede FL render-callback registration. Successful captures persist after
the capture scope closes. A device-changing operation first revokes the former
unit generation, then starts a new capture scope for the new exact device.

Before original OutputStart, the owner prepares and holds its Sink start lease,
then begins OutputStart scope with the nonzero immutable Sink generation and
a nonblocking stillPrepared callback. Hold that lease, callback context and scope
through original OutputStart and all nested DeviceStart calls. The policy copies
its decision under a nonspinning table claim, releases the claim before invoking
stillPrepared, then rechecks the same scope/unit/IOProc generation. No foreign
callback or native API runs under the table claim. Exact virtual device/UID and
Apple Start caller proof remain mandatory owner checks immediately before this
policy's approval.

Scope nesting permits metadata only for the same gate/unit/device/generation.
OutputStart cannot reenter another OutputStart. Foreign threads have distinct
fixed TLS stacks. Over-depth, different owner and concurrent unit scopes refuse
without overwriting an active scope. Non-LIFO closure revokes all affected
generations before unwinding its TLS interval. Moving a scope to another thread
does not transfer authority: foreign close revokes the unit, and the original
thread must close its own enclosing metadata interval before reusing it.

For actual DestroyIOProcID use beginDestroy first, including for unknown IDs.
Its move-only reservation spans the native owner's owner lookup, Sink mutation acquisition,
exact commitRevoke, original native Destroy and lease closure. No table claim
spans this interval. Both capture and start reject a reserved ID. The reservation
is by ID, not merely device/ID, so a mismatched device cannot bypass it.

For a known binding, The native owner obtains its Sink unit Mutation lease, or verifies an
already-held same-thread unit Mutation lease during nested teardown. If busy,
The native owner must abandon the destruction reservation without calling original. An
uncommitted reservation's close does not revoke the prior live binding. Unknown
IDs also reserve: a concurrent original Create may return that numeric ID, but
its metadata capture is refused and its unit generation revoked. The native owner cannot
start that uncaptured association.

commitRevoke checks the exact reservation token and preserved binding serial,
unit generation and device before revoking. It grants only once. The native owner invokes
original Destroy once only after commit succeeds, and retains both leases until
it returns. Native failure never restores a committed binding. Dead IOProc owner
metadata remains available after revokeUnit for cleanup; a different newly
created ID cannot overwrite it. Same-ID genuine Create after reservation close
may establish a new binding serial. Legacy revokeIOProc is metadata-only and
does not replace this native destruction lease protocol.

Revoke unit before disposal, uninitialize or device change. Call
originalStartReturned before closing OutputStart; failed native Start revokes
all captures for its unit generation. Native create failure also revokes that
generation. A new actual successful capture is required before it can start
again. Reused unit addresses obtain a new monotonic generation; old IOProc
metadata cannot regain authority merely from address equality.

Call deviceStartReturned immediately after each approved original DeviceStart
or StartAtTime. Its failure revokes the entire current unit generation at once,
before Apple can retry a different captured IOProc inside the same outer
OutputStart. A successful inner or outer result never re-arms a revoked unit.
This result API is owner-reported actual-call evidence, not permission to invoke
an original API without approveDeviceStart first.

Sink callback/format/enable mutations invalidate its separate prepared witness
under the Sink lifecycle lease; they do not inherently destroy a still-live
device IOProc. Keeping these two generations distinct preserves metadata
captured before Sink registration. Actual destruction or a changed native
unit/device relationship must revoke gate metadata.

The tables contain at most 16 units, 32 distinct IOProc identities and 16 pending
destruction reservations; the TLS stack has 8 frames. Dead IO identity history
uses its slot until a genuine Create returns that same numeric identity again.
Pool exhaustion refuses. Contended admission refuses immediately. A contended
operation that cannot safely complete revocation or bookkeeping marks the gate
poisoned for the rest of this process: later admission refuses. No spin, mutex,
heap allocation, file operation, SDK query, device call or pointer dereference
is used. Numeric statistics contain reasons/device/stage, never unit, callback
or IOProc addresses.

The independent controls compile only these policy files and the C++ standard
library. They cover missing/wrong/null identities, wrong pair and AtTime,
cross-thread and nested scopes, reentrant Start, move and out-of-order closure,
reused units/IDs, failed native create/start and retained destroy revocation,
scope/table exhaustion, failed Sink validator and validator-time revocation,
concurrent pending Destroy versus Start/capture, abandoned reservation, dead
association cleanup after a newer generation, unknown-ID reservation, committed
failure/reuse and stale/double token refusal.
They establish policy behavior only. Genuine Apple caller identity, sink lease
integration, original-once interposers and native FL callbacks require their
separate owner tests and actual runtime qualification.
