# Normal FL bridge owner contract

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


`FLAppBridge.mm` is the owner glue for the NEW standalone host/decoder/state/router
modules. `build_helper.py` compiles it without launching any DAW or plug-in.
The generated helper manifest defaults to `runtime_ready:false`; compilation is
not actual normal-application or third-party qualification.

## Admission and native binding

- Startup executable, parent PID, private run directory, and exact engine/wrapper
  seals come from `FLNativeApplication`. Startup parent proof is strict; the same
  admitted process retains its state/codec bridge if its launcher parent exits.
  A different/forked PID remains unadmitted. There is no AU owner or synthetic engine.
  Required project-save protection initializes at the real pre-original-Create
  callback, after the exact loaded engine binding/code pins exist. Admission
  earlier installs factory hooks only. A proven owned startup exits78 before any
  native Create/project load if required hooks/pins/save protection fail; a
  foreign process is never adopted or terminated.
- Native factory/component interception comes from `FLVST3NativeHooks`. A genuine
  ARA entry is queried only on the actual just-initialized native component. An
  independent provider is selected from that same native factory by matching its
  actual `factoryID`, and the coordinator enforces the SDK class-name pairing.
- An actual off-main native ARA initialize is capability-checked only through
  QueryInterface on that same native thread, released immediately, and reported
  on main as unsupported. No borrowed instance is marshaled for a late binding.
- Security signature checks are recorded as diagnostics, including failures.
  Native FL and the OS have already loaded/initialized this exact module; a
  bridge-only signature requirement is not added, and no signature is treated
  as registration/licensing proof. No vendor bundle is re-signed or repaired.
  The actual loaded framework image may differ from its bundle executable. Hook-held code leases,
  actual loaded factory image provenance, and factory refs outlive model children.
- The actual initialize context is resolved through the verified reciprocal
  component/host/plugin/effect-wrapper/native-tag/mixer chain. Two ARA components
  sharing a mixer insert are refused explicitly.

## Model and source audio

Each model/source/archive operation closes that native component's process gate,
waits for actual native in-flight SDK calls to drain, and requires raw native
transport zero. An owned worker then trylocks/unlocks the verified actual native
recursive mutex: success proves main held no outer native mix lock at that probe
instant. Contention/unknown results defer work without disabling ARA. The probe
repeats after the capture lock is released and after original-only parameter
state capture. This does not hold the native lock across later ARA work.

Empty factory/document/role binding is a separate fresh lifecycle scope, before
any sources/readers. Its outer-lock observation is recorded diagnostically; a
foreign lifecycle lock is never released. Actual borrowed-native freshness is
rechecked immediately before entry.bind after factory/DC construction. A
verified engine-file seal is obtained OUTSIDE HostLockMix; the opaque seal is then
used for bounded current RAM/direct-clock copying INSIDE the actual native mix
lock. That lock is released before graph decoding, providers, plug-in calls or
any ARA work.

The native setupProcessing call must establish float32/48 kHz. Copied source
format checks independently establish 48 kHz source PCM. Only qualified int16
mono/stereo, zero-reference, normal-rate physical sources can publish. The model
classifies the entire native channel/record manager but copies PCM only for the
actual selected mixer insert, including channels with no Playlist regions. All actual
native audio-row indices are retained for archive validation, including other
inserts. Move/trim/duplicate refresh uses the existing document controller and
one modification per actual channel/source identity.

The source overlay pointer is published once through a process-lifetime atomic
listener pointer; asynchronous ARA listeners never read its main-thread unique
owner. ARA edit callbacks immediately close source-cache tickets. Native source-read
identity changes also close tickets immediately; record/physical/pointer/source
metadata changes enqueue a stopped re-capture. A fully sealed affected read
with an unauthorized published ticket holds only its positive returned span
with silence and queues a stale-hold mailbox. Unknown/unproven sources and
native zero-read tails are not overwritten. Both overlay construction sites
provide the mandatory owner stale-hold mailbox. Unsupported shape/class/transform
faults disable the affected insert. Each publication uses exactly its copied
native model plus its decoded graph and freshly authorized cache ticket.
An actual typed empty-region graph preserves retained native source/modification
edit models in the same document. Its old table closes/removes, then a genuine
no-overlay empty cache batch authorizes the current epoch for prepared archival.
No source cache is published and unchanged polls do not invalidate that fresh
empty ticket. No retained source is guessed from an authored empty graph.

Initial native process admission remains closed until genuine source caches are
published or an actual empty graph is synchronized. After original setupProcessing
returns, synchronous priming runs if the real stopped/mutex proof passes; otherwise
it remains pending for the next qualified main boundary. An actual blocked
initial process attempt queues one visible waiting notice while preparation
remains pending, without disabling transient loading/analysis. A qualified
cache clears the notice latch. Ordinary admitted
processing still forwards the actual native processor once. No generic
assumption that an editor-role third-party processor is dry is made.

## Native component state

- Normal bytes call the actual original method exactly once, unchanged.
- Controllers strictly unwrap only ordinary component bytes; they never restore
  ARA. Their interception is enabled even before native component association.
  A recognized envelope on a controller without a proven same-record bound
  component refuses before original and leaves its stream bytes/position intact,
  while latching whole-file protection on that live native record. A separately
  initialized controller may resolve only through its actual native context,
  the sealed reciprocal H/P chain, and current P+818 component/P+828 controller
  fields. The actual component pointer locates an already-live bound record;
  the full controller/owner chain is revalidated at state calls. Records and
  lifetimes remain separate. Class IDs, factory/order and payloads never pair
  them. This source/static association still needs native cold-restore proof.
- A recognized `RYSFLAPP` envelope is strictly decoded. A current-RAM/SDK-ID-map
  preflight occurs before any ARA restore. The archive and exact actual native
  caller/load identity remain pending if FL has not published matching native
  source RAM yet. A later stopped actual model boundary performs the validated
  existing-document replacement. Pending archives remain status-visible and
  refuse actual protected project saves; they are never silently dropped or
  marked ready. Recognized unbound archives latch protection immediately even
  without a BridgeRecord, until actual native termination/final release.
- The actual state caller must resolve to the exact already-sealed owned wrapper
  or engine. This authority is retained explicitly for a deferred load window.
- Genuine archives are prepared after a qualified stopped cache/empty-batch
  build at the ordinary SDK-safe boundary. The native project-save open guard
  rejects playing/unready/direct-target/non-full-project routes before file
  effects. For every live bound component it closes/drains its process gate,
  captures actual native metadata under HostLockMix and validates its immutable
  prepared archive with pure metadata/ticket checks. The foreign save lock is
  never released and no ARA/plugin getter runs in preflight or precommit.
- Actual component getState within that exact native save transaction calls
  original getState once for ordinary bytes, then writes its frozen validated
  ARA envelope. No ARA SDK archive call occurs under the save stack. Outside a
  known project transaction, presets/diagnostics call ordinary original state.
  Protected unresolved unbound envelopes never silently serialize as plain state.
- Before the first destructive target operation, precommit requires successful
  envelope serialization from every expected live component, unchanged membership
  and fresh actual native metadata matching every prepared archive. Any failure
  vetoes target unlink/Foundation move; partial temp evidence remains. Fresh
  atomic all-native-record liveness is evaluated at each file boundary, instead
  of a main-thread bool snapshot that could erase a background observation.
  Active guard transactions stay engaged independently of current record
  lifetime; new ARA after an unguarded open must veto destructive effects
  without an admitted transaction. Immediate
  edit notifications atomically fail an active precommit transaction. An off-main
  state hook also atomically fails an active transaction without inspecting its
  stack; setters refuse before original. Null-stream refusals also fail the
  transaction. Main controller state replacement
  refuses during an actual save frame. The commit
  linearization rejects later failure marks so an old target is not stranded
  after commit has begun. Frame completion releases frozen refs/restores gates;
  it is not a claim of actual whole-file success.
- Actual ARA edit revision notifications queue a main-thread public FL SDK
  FHD_SetDirty48 call outside hooks/save frames. Only dispatcher return is
  counted; native GUI dirty/star/save-prompt behavior requires runtime readback.
  Genuine-entry/protected-envelope publication around an active Save also queues
  Dirty48 after its frame, including publications after commit has begun; ordinary
  cold-loading without an active Save does not itself enqueue dirty state.
- Exact native controller associations are published into separate stable owner
  metadata at initialized/setComponentHandler/model boundaries. The component's
  ownerData and controller/component lifetimes remain distinct. Unchanged main
  polls never temporarily unpublish a live RT route; lost/changed ownership
  closes the old owned cache before clearing its route.
- Actual successful controller `setParamNormalized` invalidates only its verified
  current ARA component. `IComponentHandler::performEdit` compares the exact
  accepted or synchronously pending handler pointer against prepublished current
  ARA controller records. A shared known handler affects only its matching ARA
  owners; the diagnostic first controller never supplies routing identity.
  Unrelated/unassociated controllers and handlers do not close ARA gates or
  queue STOP. RT callbacks read stable metadata/atomics only, with no native VM,
  SDK, allocation, logging, registry or lock. Main before-setter publication
  catches synchronous performEdit; failed setters retain their accepted old
  handler and clear/restore only the pending argument.
- Positive inputParameterChanges queues remain scoped to the exact actual
  component; empty queues and unrelated native components do not invalidate ARA.
- Ordinary state changes invalidate cached playback without clearing ARA edits.
  A real native state-load event plus changed physical graph authorizes clearing
  old edits; an ordinary preset with unchanged source graph does not.
- Owned playback children receive original-only ordinary native component state
  captured outside all ARA operations, after genuine ARA binding and before their
  activation. Nonzero reported latency/head/tail are explicitly unsupported. Before publishing
  an owned cache, actual selected native RAM/clock is copied again; a changed
  physical/PCM generation invalidates/retries instead of publishing stale audio.

Own teardown is independent of the model/render hold. A main-thread synchronous
actual native pre-original-terminate callback proves the exact retained component,
actual prior successful deactivate/stop calls, its closed own process gate and
zero actual in-flight process calls. It does not require the song to be stopped
or the global engine to remain current. A cache ticket closes immediately, and
own ARA refs are released before the native original terminate. Immutable source
tables are retired only through the router's qualified cleanup boundary.

## Status and unresolved runtime requirements

`fl-ara-status.json` and `native-app.jsonl` are own-run artifacts. Status includes
admission, actual native engine readiness, per-insert graph readiness, pending
archives, cache publication/authorization, source-read counters and errors. Own NSApp window titles/visibility/represented
URLs are read-only diagnostics, expressly not native graph proof. Optional
registration read-through reports counts only; it never reports payload values
or claims vendor license validation. Optional fixed-storage native before/after
process observation exists only with the explicit virtual test-device UID. It
reports actual numeric differences/nonzero input/hash/failure statistics without
changing native processing. In that explicit test mode the output sink keeps
actual engine render in fixed scratch and zeros only driver-facing buffers;
normal mode forwards audio. Output configuration alone is not native rendering
proof; actual render/nonzero/muted/error counters must qualify it. Write-only
callback preparations and critical-property disarms are reported. The source
never sets runtime qualification true.

Actual runtime must still establish normal GUI insertion, source cache audio,
ordinary processing behavior, native saving/cold reopening, live Playlist edits,
third-party editor actions and own teardown. The configured profile is 48 kHz mono/stereo, up to 300 seconds per source,
64 MiB per-source/128 MiB total copied PCM and 256 MiB cache; those limits are not
claims of runtime coverage (current qualification fixtures are approximately
two seconds). Saving an applied ARA graph requires native playback to be stopped;
a running project save is vetoed before output creation. File controls establish
the isolated guard/glue contract, not actual FL whole-file behavior. Native
parameter notifications, global controls and potential third-party editor-role
DSP must be measured before qualification. A vendor control that
changes DSP without ARA notifications or the public native parameter/state paths
requires additional actual evidence; no private vendor API is assumed. A refused component state method is not proof that FL refused its whole file
save; the actual host behavior must be checked before describing it that way.
The bridge does not load or select a test
plug-in and does not supply private analysis/readiness APIs. ARA/manual edits and public parameter notifications close tickets immediately
and queue an atomic stop request. Main, outside native Save, drains stale holds
and stops real FL playback with verified raw transport0, then rebuilds while
leaving playback stopped. It never resumes or guesses a seek. A stale-hold
ticket is the old published epoch; a different current gate epoch alone never
discards it. Only an already coherent authorized current generation makes a
queued request obsolete. A visible notice reports verified playback stops;
status reports pending/verified stops and held source spans. Native timing,
GUI behavior and coherent normal playing-edit behavior remain unqualified
until actual runtime tests; the source policy is now explicit hold/stop/rebuild.

`check_save_glue.py` builds UBSan own-file controls with explicit native-admission
stubs. The production preflight/precommit/finished code and actual SaveGuard
file hooks are linked. The controls verify unbound archive protection/lifetime
clearing plus all-component membership/missing-envelope vetoes preserving the
old target. They never forge opaque prepared archives or load a DAW/plugin. A
positive genuine-ARA/native-FLP save and cold reopen are separate required tests.

## Build

```
python3 build_helper.py --ara-sdk /explicit/ARA_SDK --vst3-sdk /explicit/VST3_SDK \
  --legacy-host /explicit/FLNativeModel/source --output /explicit/build
```

The new `fl/legacy/FLNativeModel.{h,cpp}` is a separate adapted copy with full
native channel classification and selected-insert PCM filtering. Historical
sealed research sources remain unchanged. All ARA host,
coordinator, state, decoder and source router code comes from this new tree.
The helper's Mach-O self ID is `@rpath/FLAppBridge.dylib`; frozen private build
paths are not its install name. The output manifest supplies the launcher's exact `binary` and `sha256` fields
but leaves all runtime capability qualification flags false. Root owns runtime
qualification and may update those flags only from actual evidence.
