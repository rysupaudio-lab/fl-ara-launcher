# Normal FL Studio application integration — static route, 2026-10-24

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


This is a read-only architecture investigation, not normal-app qualification.
No FL/Logic app or plug-in was launched, no existing process was attached, and
no installed file, preference, device, project, or audio setting was changed.
Root owns all runtime and new integration code. The earlier FL AU Lab remains
separate; it cannot count as normal FL application integration.

## Exact target and launch route

`FUN_1000040d0` creates the genuine NSApplication and its normal launcher host,
constructs `NSBundle.mainBundle.bundlePath + /Contents/Libs/FLEngine_x64.dylib`,
and passes that exact path to `FUN_100092490`. The latter `dlopen`s it and calls
the original `dlsym(handle, "CreateFruityInstance")` result. The lookup xref is
`0x100092544`; its containing function begins `0x100092490`.

Normal native call ABI, as observed statically:

```cpp
int32_t CreateFruityInstance(void* actualLauncherHost, void** actualEngineOut);
// Original function and both argument pointers must be forwarded unchanged.
```

`OsxFL + 0x1d70b0` stores the actual launcher host;
`OsxFL + 0x1d7090` receives the actual engine object;
`OsxFL + 0x1d70b8` stores the original dlopen handle.
After CreateFruityInstance returns zero, the launcher invokes the actual returned
object's vtable slot `+0xc8` with that object as its sole receiver. Further static
tracing proves this is **cleanup**, not run: TFruityInstanceA VMT `0x146b6a8`
(size0x18), slot+c8 -> `0x536f70`, clears engine global `0x1b36338`, invokes
TFruityInstanceW global `0x1b36340` slot+c8, then frees A. Do not invoke it.
CreateFruityInstance calls `0x9e40` at `0xa238` after assigning the out pointer.
`0x9e40` calls Application-like `0x5b0b0` then `0x5d4b0`; the latter dispatches
an actual GUI back-end function pointer from global `0x21a3118` with native
application `+0xa8`. This is the normal run-loop candidate; it may block until
quit. Meaning of that indirect function is not independently established here.
Therefore schedule a guarded main-thread timer **before** forwarding original
CreateFruityInstance; waiting for its return can mean waiting until app shutdown.
Only let the timer inspect after the actual native out pointer becomes nonnull
and exact engine/class/owner checks pass. No native methods may be called from
an inferred ABI.

Installed signing entitlements explicitly include both
`com.apple.security.cs.allow-dyld-environment-variables=true` and
`com.apple.security.cs.disable-library-validation=true`, plus get-task-allow.
These permit a plausible signature-preserving private-copy DYLD route. This
does not establish that a future macOS version or FL build will accept it.
Apple documents the interaction between the two DYLD/library entitlements:
https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.cs.allow-dyld-environment-variables

## Actual clock and model authority

The existing reader already validates actual native classes and exact live
Playlist/source bytes. Its arrangement/channel/source offsets remain applicable
to the same sealed engine whether hosted as AU or normal app. New standalone
authority must establish its own lifecycle, actual native owner, and clock.

| Global | Type | Current native evidence |
|---|---|---|
| `engine+0x1ba1fd8` | PPQ u16 | `0x407550..0x40755c` loads into FLhd header word at local offset 12; `0x407560..0x407580` writes 14 header bytes |
| `engine+0x1ba1f48` | project milliBPM int32 | `0x407c5c..0x407cd8` emits native event 156 payload |
| `engine+0x1ba1f60` | distinct host tempo | Earlier AU source observation; must not replace project tempo |

The PPQ address is **0x1ba1fd8**, not a guessed shift of old FL25 addresses.
Header emitter is within native save function `0x407410`; native event loader
is `0x40fbb0`. Event156 import at `0x412cdc` checks `engine+0x1e36606` (tempo
slaving) and dispatches the original event word through the actual tempo object.
Those routines do not alone identify the whole project transaction boundary.
The named full open/save functions and native audio suspend/drain entrypoints
remain unknown; none may be called or hooked on an inferred ABI.

First implementation should read actual project PPQ/milliBPM on the model thread,
with engine seal, finite/range checks, and two equal complete graph captures.
They are model authority; VST3 ProcessContext supplies actual playback time,
sample rate and playing state while the native processor is called. Its context
position may include PDC and is not automatically the source-reader coordinate.
Read validity flags; stale last-process state is not proof of a current hold.

## Standalone lifecycle design

1. Admit only an exact user-owned private app copy, sealed launcher/engine and
   explicit parent PID/run UUID; forbid `/Applications` and pre-existing PIDs.
2. Interpose the genuine launcher `dlsym(CreateFruityInstance)` lookup and
   forward exactly once. Capture both original arguments and original out
   pointer, whose contents the real native function assigns before its GUI loop.
   Verify actual engine/host reciprocal links and loaded engine path.
3. Schedule a guarded main-thread run-loop timer before forwarding original
   CreateFruityInstance; it may run only after genuine normal engine startup.
   It can inspect and poll; a timer alone is not an exclusive FL model lock.
4. Reuse proven actual child VST3 initialize/native-owner association, component
   state wrappers and native pre-declick source-read slot, after new explicit
   standalone admission. Do not manufacture AudioUnit parent arguments.
5. Close overlay admission before graph/cache replacement; atomically deny new
   overlay readers, drain their in-flight leases, then edit ARA documents outside
   native mutation recursion. Admit only an actual stable live graph generation.
6. Reconcile add/remove/move/trim/route/undo to stable source/modification/region
   IDs. Preserve edits on identity-preserving move/trim; invalidate required
   source renders on edit/source change. Remove stale refs before source release.
   Unsupported stretch/streaming/SRC must be refused explicitly before playback.
7. At each native child getState/setState record actual stack/lifecycle phase.
   Whole-project native load must provide actual input bytes, not a diagnostic
   reserialization. Old AU ClassInfo attestation cannot be reused or invented.
   Cold normal-app reopen must prove real file + ID relocation + PCM identity.

The source cache seam is before native clip declick, fades, gain/pan and mixer.
A whole-mix replacement or visible AU-host window does not satisfy this route.
Full native save/load transaction exclusion remains a required observation.

## Private preferences and silent launch

The previous AU linked-home helper shadows only Image-Line/reg.xml; all other
preferences and Documents entries are symlinks to real user state. That is not
sufficient for the normal app, which can write its shared CFPreferences engine
path, user data, window state and caches. Shadow/copy all Image-Line preference
domains into private files; keep private Documents/Image-Line and caches real,
not links to Jordan's folders. Do not change the installed bundle ID or sign it
just to get isolation; its normal signature can be retained in a private copy.

Official Image-Line documentation confirms user data includes projects, plugin
database and custom settings, and moving it can migrate data. Rewrite only the
private copied registry; never use the UI to migrate Jordan's real folder:
https://ww2.image-line.com/fl-studio-learning/fl-studio-online-manual/html/envsettings_files.htm

Read-only CoreAudio enumeration on 2026-10-24 returned a real existing virtual
device `Pro Tools Audio Bridge 2-A`, device ID 119, UID
`Pro Tools Audio Bridge 2-A_UID`, transport type `virt`. IDs are runtime-scoped.
Other existing Pro Tools bridge devices and Zoom are virtual; no BlackHole driver
is installed. Do not use aggregate/multi-output devices: they may feed speakers.

Normal FL26 registry currently has `Devices/Audio output/Device name` with value
`4Default audio device` and normal Sample rate `44100`; plug-in-specific rate is
separately `48000`. Thus the existing plug-in-rate helper does NOT fix normal
app rate. Proposed private normal device string `4Pro Tools Audio Bridge 2-A`
must still be confirmed by the native app; `4` prefix comes from actual registry
observations but is not a public API contract. Choose 48 kHz for routine tests.

Before any launch, install a child-only fail-closed CoreAudio output admission:
resolve exact UID and transport `virt`, whitelist that identity and reject actual
hardware/default fallback starts. Cover AudioDeviceStart/StartAtTime and HAL AU
output start paths used by the app. No global device changes, mute or hidden
speaker playback. A missing virtual device aborts tests instead of falling back.
Virtual device selection alone does not prove absence of an external monitor.
Image-Line describes the app-local CoreAudio device selector, with no documented
silent/no-device driver option:
https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/envsettings_audio.htm

## Minimum runtime qualification root must collect

- Exact private OsxFL image/engine creation and normal FL main window; installed
  byte manifests and real preference pins unchanged before/after.
- Prove private preferences and selected virtual output before loading media.
- Genuine native open + saved filename + child state/graph timing, then actual
  native Save As + cold normal-app open; preserve failed observations as failures.
- Visible native plug-in editor on a real mixer insert and actual ARA sources;
  original ProcessContext/native source-read callbacks prove Play/Stop/seek.
- Move/trim/add/remove/undo/re-route while stopped and during playback; overlay
  never uses a cache for a stale native record or stale PCM generation.
- Actual third-party ARA editor edit, audio read, render, archive and cold reopen;
  UI loading or ARA factory discovery alone is not compatibility proof.
- End-state cleanup leaves no owned DAW, bridge, analysis process or write lease.
