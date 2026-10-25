# Stale ARA playback: hold, verified stop, rebuild, user restart

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


Source/offline control contract only. No native application was launched, native
STOP called, or audio device used to verify this new policy.

## What happens after invalidation

The coordinator invalidates its shared cache epoch immediately on actual ARA
notifications and manual graph/cache invalidations. It then makes the queue-only
`StandaloneOwnerAdapter::queueActualNativeStopForCacheRebuild(instanceID, epoch)`
request. The callback must not call native/ARA SDK, wait or render. Contract/insert
failure paths also request this owner-controlled refusal boundary.

The source router forwards the original native read exactly once. Before writing
anything it performs its existing exact known-target checks: unique actual region,
channel and mixer association; engine/class seals; exact native record; physical
source/sample/reader/PCM pointer relationships; source/reader format; actual output
buffer/returned count; source position and span bounds. Unknown, ambiguous or
unproven reads are never silenced or substituted. Target failures keep the native
buffer unchanged and use the existing fault path; unrelated sources remain original.

For a fully proven affected source whose cache ticket is stale, the returned
positive-frame interleaved span is set to silence instead of forwarding dry source
PCM. Guard/padding/tail samples outside that returned span and native count/flags
remain unchanged. A native zero-frame result queues the hold without touching any
sample. No stale cache sample pointer is formed on that path. A final ticket check
after copying a valid cache turns an observed mid-copy invalidation into the same
silence hold.

The router returns `HeldForRebuild` and records `heldCallbacks`, `heldFrames` and
`staleHoldRequests`; the instance exposes `staleHoldActive`. This is an expected
rebuild-needed state, distinct from identity/format faults.

## Stable model-thread mailbox

R3 configuration requires `ownerStaleHoldQueued`, a stable owner-owned atomic
mailbox. The RT router only stores true. Main drains
`takeStaleHoldsOnModelThread()` for exact instance/component/capture/ticket records.
There is at most one request per stale publication episode even across repeated
reads and mailbox drains. A coherent new publication clears the hold and rearms
one request for its own later invalidation.

The RT path allocates nothing, calls no owner/native/SDK function, does no file
work, and waits on no lock. Writes are bounded by the independently checked native
returned count, no larger than the supported native block bound.

## Native owner integration

The bridge must coalesce direct observer requests and RT hold records. On its
actual main thread, outside an active Save transaction:

1. Verify the exact record is still alive and affected; ignore obsolete requests
   when the coherent current overlay/no-overlay generation is already ready.
2. If actual native transport is playing, call the verified native STOP once and
   verify actual stopped readback. Mailbox delivery/dispatch success is not STOP
   success. Never release an outer native lock belonging to another caller.
3. Close/drain the actual native component process gate, copy current RAM/clock
   under the real native mix boundary, then release that lock before ARA work.
4. Reconcile/poll/rebuild at the actual stopped boundary. Keep the source gate
   closed for retryable analysis; show a stopped/rebuild-needed state. Publish
   only a complete current generation (or genuine authorized no-overlay state).
5. Leave transport stopped for the user to restart. No automatic resume intent
   is inferred. If STOP/readback or reconstruction fails, expose refusal and
   retain the hold for proven sources; do not silently resume dry playback.

Actual native document-dirty forwarding and prepared Save/commit rules are
unchanged. The bridge must not execute a native STOP from inside Save handling;
late invalidation instead participates in that transaction's existing refusal.

## Exact limitation

A queued main-thread STOP cannot retract audio already mixed, copied, in flight
or buffered by the device. Invalidation can occur after the final per-read check.
This policy prevents subsequent fully proven stale reads from deliberately
forwarding dry source PCM; it does not claim sample-immediate global stop or a
fixed wall-clock stop latency while the native main loop is busy. Unknown or
failed-proof sources are deliberately not muted. Actual native STOP/readback,
visible status, rebuild and restart behavior require native runtime qualification.

Offline UBSan controls cover positive-span silence, untouched guards/tails/flags,
zero-read preservation, strict failed-proof refusal, unrelated source isolation,
one queued exact-owner request, counters, generation rearming and zero RT
allocations. They do not prove a real transport stop.
