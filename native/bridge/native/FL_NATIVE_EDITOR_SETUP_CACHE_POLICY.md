# Native editor-only setup and source-cache authority

Native editor setup can change processing configuration without editing the ARA source model. This contract defines bounded cache continuity; actual normal-app export and audio remain unqualified.

`FLAppBridge::setup` now invokes the SDK-free
`FLNativeEditorSetupCachePolicy.h`. It preserves an existing cache only when:

- The exact original setup succeeded, and a previous setup was qualified.
- The actual new setup is 48 kHz, float32, legal mode 0/1/2 and a positive block
  size at most 65,536 frames. This bound is policy admission, not runtime coverage.
- The callback is on the actual model/main thread outside native processing.
- The bridge's existing full current-cache coherence predicate succeeds. That
  predicate includes live owner, applied graph, no dirty/fault/teardown/prime or
  pending archive, prepared save authority and its actually authorized ticket.

Only already-bound editor-only components reach this callback. The coordinator
binds them with `EditorRenderer | EditorView`, while full-source PCM is rendered
by a separate owned playback companion, with its own block and offline setup.
The local VST3 SDK explicitly requires `setupProcessing` for realtime/offline
mode switches (`ivstaudioprocessor.h`, Process Modes switching documentation).
That configuration call alone is not an ARA source/modification edit.

The preservation path records the actual native rate/qualification and returns.
It never publishes/reopens a gate, clears dirty, resets initial priming, changes
source/cache data or performs model/native/ARA work. If an actual editor change
closes the gate after the coherence read, that gate remains closed and its dirty
and stop/rebuild mailboxes remain set. The first, unready, incompatible or failed
setup retains the existing invalidation and actual stopped-boundary rebuild path.
Unknown modes, precision and invalid block extents are not qualified.

Candidate 9's first-32 immutable setup diagnostic slots and early-return RAII
completion remain intact, as do all other bridge/source/processing/save guards.

`FLNativeEditorSetupCachePolicyTests.cpp` uses the actual shared cache gate and
edit-invalidation/dirty-revision helpers. It exercises the measured configuration
sequence, strict failures/unready cases, a deterministic edit after the coherent
read and concurrent edit/setup decisions. These are offline adapter simulations;
they load no native app, ARA factory, plug-in or audio device. Actual candidate 10
normal-app export, edited audio, save/recall and third-party qualification remain
subject to exact-binary runtime tests.
