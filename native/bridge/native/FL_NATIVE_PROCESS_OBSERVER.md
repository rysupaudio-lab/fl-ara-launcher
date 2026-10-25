# Test-only native processor differential observer

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


`FLNativeProcessObserver.h/.cpp` observe the existing native hook's synchronous
`BeforeOriginal` and `AfterOriginal` telemetry. No plugin or DAW is launched by
this module. The observer never changes ProcessData or sample buffers, calls an
SDK interface, performs file work, schedules work, or dynamically allocates.

Create one `process_observer::NativeProcessObserver(record)` on the model thread
after the stable `InstanceRecord` has its final identity, only for explicitly
enabled test telemetry. Store it in the existing stable owner payload. In the
native telemetry callback, use that direct payload pointer and call
`observer.observe(record, telemetry, thread)`. Both phases must reach the same
observer. Do not find it with a map or create it on the audio thread. Destroy it
only after closing the native process gate and draining native calls. The hook
itself suppresses telemetry for closed gates and owned-companion TLS bypass.

The fixed scratch storage holds two planar float32 channels of at most 8192
frames (64 KiB). It copies actual inputs immediately before the genuine native
processor call, so in-place buffers cannot overwrite the reference. After that
exact call, it compares actual outputs, once. No plugin rendering is introduced
by this observer. It supports exactly one input/output bus with equal mono or
stereo channel counts. Different layouts, float64, oversized/negative blocks,
null non-silent channels, changed shapes and failed native calls are counted
and excluded. Zero-frame blocks are counted separately. Silent null channels
have numerical value zero. When actual buffers exist, their raw samples are
read even if a silence flag disagrees; those contradictions are counted.

The observer checks stable record/processor identity and the real native thread
token. One nonspinning atomic claim prevents simultaneous writers. Concurrent
callbacks are skipped and counted; same-thread nested pairs are skipped without
releasing the outer pair. Interference counters use one bounded saturating CAS
attempt, so they are monotonic lower bounds under contention. They cannot make
an observed interference event look like a successful compared pair.

`trySnapshot(snapshot)` is for a non-RT thread. It makes at most eight attempts
to read one coherent committed pair-statistics epoch; false leaves the caller's
output unchanged. It never reads the RT scratch storage. The two independent
interference counters are sampled separately. No reset or observer replacement
is permitted while processing can occur.

Numeric output includes compared frames/samples, exact bit-difference count,
FNV-1a hashes of the observed float bit patterns in channel/frame order, maximum
absolute difference, difference sum of squares, input/output energy, nonzero
input count, finite/nonfinite counts and explicit failure/skip counters. NaN and
infinity are hashed and counted but excluded from the numeric sums. Counters
saturate rather than wrap and record saturation attempts.

An equality claim requires meaningful non-silent compared input, matching
hashes, zero differing samples, and zero failures, unsupported/limit/identity/
unpaired/concurrent/nonfinite/overflow flags over the reported test window.
The RMS difference is `sqrt(differenceSumSquares / finiteSamples)` when that
denominator is nonzero. Exactly equal raw processor input/output establishes
pass-through only for the tested native plugin, settings, input and intervals.
It does not establish general vendor behavior, ARA sample provenance, latency
alignment, or an entire application integration.

The standalone controls use synthetic public SDK ProcessData only. They cover
dry/gain and in-place comparisons, mono/stereo, bus/shape/format limits, null and
silent channels, maximum/zero/negative blocks, nonfinite values, failed calls,
reentrancy/concurrency, counter saturation, allocation checks, and coherent
publication under a 10,000-pair concurrent reader.
