# Normal FL transport and real native hold — source map, 2026-10-24

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


## Current playing state, including after final audio callback

Canonical int32 state is **engine+0x21a2354**. Never infer stopped from missing
process callbacks. Native getter `0xa10700` returns `(state == 1)` directly.
Actual public Python `transport.isPlaying` is method-table entry `0x1c52438`:
name string `0x11d0110`, function `0x81d6f0`, method flags4. It returns the
**raw signed native int** from that global via PyLong, not a cached process flag.
Conservative model/PCM capture must require **raw state == 0**. A raw2 state
must not be accepted just because native bool-getter says false.

Native updater `0x310d90(int32 newState)` stores its parameter to that global at
`0x310eb8..0x310ec0`, then notifies through `0x37ca60` and other native paths.
Only a source mapping, not permission to call this internal low-level setter.

The actual notification chain is independently visible:

1. Engine `0x37dbd8..0x37dc04` compares current global with1 and invokes original
   plug-in dispatcher with ID12, index0, value=current Boolean.
2. Actual TEffectWrapper `0x64c60` jump table `0x65a94` entry12 -> `0x65404`;
   stores the value Boolean to native wrapper W+0x805, then invokes its actual
   child TVST3Plugin pointer W+0x50a8, vtable slot+0x218.
3. Actual TVST3Plugin VMT `0x2afb80`, slot+0x218 -> `0x78b40`, stores W1 to
   P+0x6fc. Native process `0x85c20` builds ProcessContext at stack+0x50;
   `0x860c0..0x860d8` reads P+0x6fc and sets state bits2 and4.

Thus a main-thread state read can cross-check current global, actual W+0x805,
and P+0x6fc on the already-proven native owner chain. Polling global zero plus
actual callback drain is stronger than retaining the last ProcessContext. The
wrapper's bit4 behavior is observed; do not infer recording from it.

## Native play and stop requests

Actual local public SDK source:
`Apps & Tools/Stock Plugin Bridge/vendor/fl-sdk/fp_plugclass.h`, FHD_Transport79.
It explicitly supports only Play and Stop and returns the native handling code.
Official Image-Line constants document Play10 and Stop11:
https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/midi_scripting.htm

Actual native host VMT `0x140a818` slot+0xc8 -> `0x425f60`, original ABI:

```cpp
intptr_t Dispatcher(void* actualHost, intptr_t actualTag,
                    intptr_t id, intptr_t index, intptr_t value);
```

The opcode jump table is `0x42bda4`; entry79 -> `0x42ad3c`. That branch explicitly
accepts only index10/11, then forwards to native GlobalTransport `0x5e9b20` with
command=index, value=original value, PME_System2, GT_Global8, trailing flags0.
It returns the native signed handling result; a delayed result is not completion.

`GlobalTransport` table `0x5eaebc` entries10/11 dispatch to:

- Play `0x5e9fdc`: requires value>0; if current state==1 calls native control
  `0x311e80(2,0)`, otherwise `0x311e80(1,0)`. This is a toggle; ARA requestStart
  must first read actual state and must not toggle a currently playing app.
- Stop `0x5ea018`: value1 calls native control `0x311e80(0,0)`. Value0 or other
  values take different menu/message paths. Use actual dispatcher79,index11,
  value1 and read back actual raw state0 plus drained callback leases.

The public Python transport.start path uses command10/value2; stop uses
command11/value1. This corroborates press values, but start is still native
toggle behavior. Start/stop must be queued to actual own main thread outside
component/host callbacks. No fake key events or global/user FL process required.

## A real native capture hold

The public SDK says LockMix_Old prevents new voice creation and rendering.
Current native host slots and backing code corroborate actual synchronization:

| Host slot | Actual function | Operation |
|---|---|---|
| +0x100 | engine+0x42cd90 | loads actual global+0x1f20ab8, calls+0xa7d300 |
| +0x108 | engine+0x42cdb0 | same actual object, calls+0xa7d330 |

Global+0x1f20ab8 is initialized at `0x30bed8..0x30bef0` using constructor
`0xa7d3c0` and actual VMT `0x17187e0`. That VMT has name TCriticalSection and
instance size0x48. The lock helper dispatches TCriticalSection slot+c8 ->
`0xa7d380`, which passes object+8 to FPC EnterCriticalSection `0x29770`; paired
unlock dispatches slot+d0 -> `0xa7d3a0`, then LeaveCriticalSection `0x297b0`.
Those primitives dispatch through the actual FPC thread manager at
global+0x21ab608 offsets+0x88/+0x98. Same exact native object is locked inside
the normal native seek function (`0x325e70..0x325e7c`).

Do **not** use LockMix_Shared_Old (+0x1d8/+0x1e0): current native entries point
to actual no-op0x26ed0. SuspendOutput (+0x220/+0x228) only removes one plug-in
from processing lists; it is not a whole-graph hold.

Candidate capture protocol, still requiring controlled runtime proof:

1. Actual own main thread, raw transport state0, no recursive native callback,
   no project load/save transition, no outstanding ARA playback/model operation.
2. Close admission for own source overlays and new child processing leases.
3. Call the original verified actual native host LockMix_Old method. Verify the
   exact live TCriticalSection global object/VMT before calling.
4. Once acquired, own in-flight source/child reader leases must be zero. Copy
   bounded native model and qualified resident PCM, validate stable bytes/clock.
5. Unconditionally pair UnlockMix_Old via RAII, then perform expensive ARA model
   updates/cache rendering **outside** the native lock while overlay admission
   remains closed. Revalidate model generation before publishing the cache.

Never pump the event loop, render a child, run a plug-in GUI callback or wait for
main-thread work while holding the native mix lock. Main-thread callback drain
must be asynchronous; the native lock is acquired only outside active callbacks.
Threaded source loading and background model mutations still need exclusion or
strict format/source stability refusal. This is a native hold design and exact
ABI map, not an observed successful standalone hold yet.

## Native seek route

FHD_Transport79 has no seek operation in this SDK/native branch. Do not invent
an opcode or overload GetPlaybackTime. Public transport.setSongPos provides an
independent current native source route:

- Method table `0x1c52398`: name string `0x11d0030`, Python wrapper `0x81cf70`,
  flags3; its scheduled native operation is callback `0x81ce50`.
- Callback first invokes actual AppMain+8 vtable+0x160 permission check and
  refuses with result-2 if it is false.
- mode1 seconds multiplies by actual double1000 at0x11d0280, calls native
  milliseconds-to-ticks converter `0x7d31b0`, then calls native seek `0x325d60`
  with W0=flags0 and D0=double native ticks.
- mode0 is milliseconds; mode2 passes native absolute ticks directly. Mode-1
  computes a normalized position from actual current project bounds.

Root may qualify the exact native seek candidate ABI `void(uint32 flags,
double nativeTicks)` on the own main thread while preserving the same native
permission condition. The current converter uses actual native global clock
values; do not replace it with a constant BPM formula. The native seek takes its
own real mix lock and updates native UI/model state. Do not call it while an
ARA document edit or native hold is open. Verify actual readback and source
overlay position through original FL callbacks before claiming seek support.

Cycle-range/loop requests, arbitrary streaming or tempo automation remain
unqualified. Do not advertise those callbacks as supported from these findings.
