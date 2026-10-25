# Explicit QA output sink

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


`FLTestOutputSink.h/.cpp` implement an opt-in output presentation sink. They do
not create, initialize, start or stop any AudioUnit or audio device. Normal mode
must bypass this module completely in the owner's interposers.

Create one process-lifetime `test_output::FLTestOutputSink` before registering
the actual engine output callback. Call `initialize(Config)` once after native
application admission. Config requires the actual admitted engine path and the
literal `Pro Tools Audio Bridge 2-A_UID`. The owner supplies its current admitted
process predicate and the existing actual-device verifier, which must check
that exact UID and virtual transport. Real functions default to public AudioUnit
APIs; the function table permits synthetic tests without an actual audio unit.

In explicitly enabled QA mode, route the AudioUnitSetProperty interposer through
`sink.observedSetProperty(unit, property, scope, element, data, size)`. It forwards
unrelated properties and non-output components unchanged, once. It wraps only
SetRenderCallback/Input/0 or the sealed native FL Output/0 route on a public
AudioComponent of output type, requiring
the native callback's canonical dladdr image path to equal the admitted engine.
The actual FL profile also pins the callback's engine-relative offset to
`0xcd7bc0`. A zero offset is reserved for independent synthetic/metadata profiles.
No instructions, executable memory, JIT code or callback ABI changes are used.
There are eight fixed process-lifetime records, each with 128 KiB scratch. A
record is never reused for a different callback/refcon; unsupported replacement
or pool exhaustion is refused. Null callback removal disarms the record.

Before calling real AudioOutputUnitStart, the owner's QA branch must require
`sink.tryPrepareMutedOutputStart(unit)`. That requires our successful original
wrapped-callback registration witness, current exact device, input/0 client
stream format, and MaximumFramesPerSlice. Supported formats are 48 kHz, packed native-endian
float32/float64, mono/stereo, planar/interleaved, with at most 8192 frames. A
record's shape is immutable after first preparation. Failure means DO NOT start.
Direct AudioDeviceStart/AtTime must remain refused by the owner's QA branch.

Apple's actual SDK declares SetRenderCallback as Access: Write. Real AUHAL
metadata tests returned InvalidProperty (-10879), zero bytes, for its getter;
GetPropertyInfo succeeded with 16 bytes and writable=true. For that exact
write-only result, preparation requires an actual Apple output component and
that writable exact-size metadata. This is registration provenance, not a
claim of callback readback. `writeOnlyCallbackPreparations` records this case.
If a callback getter is available, the wrapper and record refcon must match
exactly. Every refused output callback registration (bad data/size, wrong image,
changed callback/refcon, failed original setter), null removal, or unsupported
scope revokes the witness and disarms the record. Unsupported output callback
scopes are refused rather than forwarded. The owner must route every relevant callback setter
through the interposer; an unobserved bypass cannot be introspected with this
write-only public API.

The native Output/0 route keeps its exact successful setter witness. Its getter
must return InvalidScope (-10866) with 16 bytes, and property info must return
InvalidScope with zero bytes and writable=false. Preparation then requires
Input/0 getter InvalidProperty (-10879) with zero bytes and successful Input/0
property info with 16 bytes and writable=true. Every different status, size or
writable result is refused. This is metadata alias admission, not callback
readback. The record retains its original scope/element; route changes are
refused. Exact Apple output identity, engine callback, test-device UID, 48 kHz
client format and maximum-slice checks remain mandatory before Start.

Status includes fixed numeric registration, metadata and output-start snapshots,
with coherent sequence and dropped-update indicators. They describe admission
steps and expose no callback pointers, refcons, paths or audio. An accepted
metadata snapshot alone does not prove that later device/format guards passed,
that Start succeeded, or that native rendering occurred.

Observed changes to CurrentDevice, StreamFormat, MaximumFramesPerSlice,
EnableIO, channel map/layout and element count disarm an existing record before
forwarding the original setter once. `criticalPropertyDisarms` counts an armed
record being disarmed. The unchanged callback witness survives those setters,
but a fresh prepare is mandatory, and a changed admitted stream shape remains
unsupported. A callback which already holds the rendering claim finishes into
scratch and clears its captured driver buffers; disarming never cancels it or
changes the immutable RT format storage underneath it.

Each supported callback claims its record with one nonspinning atomic operation.
It passes the original callback the same native refcon, flags pointer, timestamp
pointer, bus and frame count, exactly once, with a preallocated same-shape scratch
AudioBufferList. It never passes the hardware buffers to that callback. Original
driver buffer descriptors are captured beforehand. After native rendering, the
sink reads bounded scratch/returned-alias samples for numeric statistics, zeros
only the captured original driver buffers, restores those driver descriptors,
and sets OutputIsSilence. Returned native/internal aliases are never zeroed.
The native callback result is preserved when it fails.

Unsupported or concurrent callbacks do not call the native callback; they mark
OutputIsSilence, zero bounded driver buffers, and return an error. Zeroing is
limited to two buffers and 256 KiB total; impossible/unbounded driver metadata
is counted as unsafe and rejected, never traversed as an unbounded RT write.
Such a failure is not successful physical-mute evidence. Accepted stream shape
and maximum-slice checks happen before Start, and an actual QA run must have
zero unsafe/unsupported/concurrent/native-error/returned-shape/overflow counts.

`snapshot()` returns fixed numeric diagnostics and independent monotonic counters: callbacks,
native render calls/frames, nonzero/nonfinite scratch samples, muted frames and
explicit registration/start/RT failures. Counter updates make one bounded CAS
attempt; under concurrent device activity totals are lower bounds. Saturation
attempts set the overflow flag instead of wrapping. No audio is written to disk.

Physical output in this QA mode is deliberately silent. Nonzero scratch and
genuine native rendering can be reported separately; they do not prove ordinary
physical presentation. Offline native file export is not intercepted by this
module. Its actual preservation still requires native application testing.

The standalone tests use fake API functions and a synthetic callback only. They
verify original-once forwarding and pointer arguments, scratch separation,
exact hardware zeros, untouched returned internal aliases, numeric counters,
float/shape/UID/start admission, maximum and overflowing frame counts, pool
exhaustion, no allocation in valid callbacks, and nested/separate-thread gates.
No AudioUnit or device is created or started by those controls.

The separate explicit `FLTestOutputSinkAUHALProbe.cpp` CLI created an owned,
uninitialized/unstarted Apple AUHAL in each mono/stereo process. It changed only
that unit's CurrentDevice and client format, installed the wrapper, checked
actual property metadata/readbacks and preparation, then disposed the unit.
Both passed: 48 kHz, 1/2 channels, maximum slice 512, write-only preparation 1,
dispose 0, native/sink callback counts 0. Read-only device-running state remained
0 before and after. No Initialize, Start, Render, or device IO APIs occur in the
probe. Its result establishes Apple AUHAL metadata admission only, not native
FL behavior, audio output, or ordinary physical presentation. The initial
strict-getter failure is preserved as a negative control.
