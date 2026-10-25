# Native FL Save failure propagation — hard safety gap, 2026-10-24

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


**Returning kResultFalse from IComponent.getState does not abort native FL
project saving. The native FruityWrapper omits component state and continues.**
Current bridge refusal while playing, archive-pending, disabled, off-main or
outer-lock-unavailable therefore cannot be described as a safe project Save
veto. A project can be written without the component/ARA envelope. This is a
source-proven release blocker; no destructive runtime test was attempted.

## Actual component failure path

Actual TVST3Plugin VMT2afb80 slot+270 (data pointer2afdf0) -> method87e10..890a0:

1. At87f08..87f7c it creates an actual IBStream adapter with a cleared native
   memory stream and position0.
2. At87fa4..87fcc it loads the actual IComponent from P+818, then calls its
   standard vtable+68 getState method. The returned W0 is saved at SP+6c.
3. At88110..88118, any nonzero result branches to88178. This skips the component
   chunk writer at8811c..88174: rewind, actual stream size, raw bytes, chunkID3.
4. At88194 onward it continues with actual controller state (controller+38 call
   at88268), other parameters and remaining serialization. It does not throw,
   propagate a component failure to its caller, or restore old component bytes.
5. At89034..8909c it performs ordinary cleanup and returns without a stored
   meaningful save-success result. The enclosing serializer interface is void.

Source-equivalent branch (explanation, not recovered original source):

```cpp
auto result = actualComponent->getState(actualNewEmptyIBStream);
if (result == kResultOk)
    actualChunkWriter.write(3, actualStreamLength, actualStreamBytes);
// Failure still reaches this code:
if (actualController)
    serializeActualControllerState();
serializeOtherNativeState();
```

The SDK's native TFruityPlug::SaveRestoreState interface is explicitly void in
`Apps & Tools/Stock Plugin Bridge/vendor/fl-sdk/fp_plugclass.h` (GUI-thread method).
Current TEffectWrapper VMT+e0 ->68e70. Save branch68f94..68fc4 writes version12
then calls outer wrapper serializer692a0. That writes ordinary wrapper metadata,
calls P.vtable+270 at69b70, and performs no return-code check. At69bd8..69c38 it
emits the nonempty outer wrapper memory stream as chunk35. Header/controller
data can keep that stream nonempty despite a missing component chunk3.

## Native project file transaction

Exact engine ARM64 seal remains24b368d08cc861a1bf06ee8883233aa9ca579746825481bfe89f94e8e20eff2c.
Native file-save wrapper419bb0 receives filename in X0, save type W1, context X2,
flags W3. It passes a real stream to the main FLP writer407410.

When flags bit0 is clear it appends actual immutable suffix `~` (Unicode literal
11878c8) to the target name at419c0c..419c20. It creates TBufferedFileStream
(VMT170e7d0, size40, constructora63cb0) with modeff00 at419c58..419c70 BEFORE the
main project writer call419cac. Successful native writer return0 then reaches
replacement/commit helper320700 at419d30. Flags bit0 follows a direct-target
path, so a temporary-file assumption must not cover every native save mode.

Commit helper320700 can back up/move or delete the previous target before the
final move from the temporary path at320928..320950. Deletion branch3208c0 calls
CAD20 ->F7960 ->actual imported unlink stub11433b4. The move path callsFAD10 ->
FA0F0 ->A7B0A0 ->actual NSFileManager `moveItemAtPath:toPath:error:` (selector
reference1b1d9f0 points to127f39b). Therefore intercepting only a final raw rename
after a component refusal is insufficient protection: that hook may not cover
the actual Foundation path, and the old target may already have moved or been
removed. Its final Boolean failure does not undo earlier effects by itself.

These file-save and commit functions have no observed aligned static data
function pointers in the sealed image; direct function calls are present.
Do not label an unimplemented vtable replacement as an available preflight seam.
The engine actually imports open, unlink and rename, but no IO interception was
implemented by this investigation.

## Required architecture before qualification

- A component save must emit its full valid envelope or the ENTIRE native save
  must be refused before opening/truncating/committing a target file. An error
  string plus kResultFalse does not meet this requirement.
- Precompute ARA archives at genuine stopped, outer-lock-clear native boundaries.
  During native serialization, a prepared envelope can be written without SDK
  calls only if actual current graph, PCM, identities and edit generations still
  match its strict seal. Do not substitute stale last-success state.
- If readiness cannot be proved, a genuine upstream native command/transaction
  preflight must stop/defer Save before file effects. It must cover Save As,
  autosave, command actions and direct-target modes; a single UI button veto is
  not complete coverage.
- An IO guard, if selected by root, must be tied to the actual native file-save
  entry/owned process and check BEFORE file creation/replacement, with original
  failure semantics. Last-rename denial alone is unsafe for the flow above.
- Real owned normal-app Save while playing, archive-pending and forced boundary
  refusal must preserve the old file and ARA bytes. Native-save completion must
  not be claimed before those tests and cold reopen pass.

This investigation performed no DAW/app/plugin launches, no forced UI actions,
no real save and no project mutations. The user's real installations and files
remain untouched by this work.
