# Exact default-preview ownership exclusion

This narrow classification is bound to the supported ARM64 engine SHA256 `24b368d08cc861a1bf06ee8883233aa9ca579746825481bfe89f94e8e20eff2c`.

Engine initialization constructs `TPreviewChannel` (VMT `+0x150c280`) and stores
that object in the default-preview global at `+0x1e36700`. The native default
preview/render-sound dispatch uses that owner. This is a sibling of the admitted
Playlist `TPublicChannel` (`+0x150c000`), not a Playlist source class. The separate
preview pool at `+0x1e36708` can contain other objects with the same preview class;
those objects are deliberately not admitted by this default-only rule.

After forwarding the original native callback once, the existing observation
now brackets its full metadata capture with two bounded pointer reads of the
exact default global. `globalPreviewOwnerReadCoherent` is true only when both
reads succeed, are nonnull and are equal. `globalPreviewChannelObject` retains
the successful second value, or zero on a failed second read. There are no
additional file, SDK, UI or native target-function calls.

Under the existing protected-table `ReaderGuard`, the router first performs its
existing physical-channel pointer lookup across **every** captured insert. It
returns original-only `Unrelated` before clip/index alias matching only if:

- no protected captured physical source owns the observed channel pointer;
- the full native observation and record are complete, and actual voice, reader
  and channel objects are nonnull;
- the observed voice VMT is the exact native source-voice VMT;
- the observed channel VMT is exactly the sealed `TPreviewChannel` VMT;
- the two current default-global reads were coherent and equal that channel.

The original output bytes, returned count and ended/changed flags remain intact.
No insert gate, fault or stale-hold mailbox is touched. Borrowed clip IDs, channel
indices and native88 records do not nominate a Playlist owner on this path.
Mixer0, sample shape/rate, reader step, stretch fields and native88 flags are not
ownership authority. Incidental native preview44100 metadata does not extend the
48k ARA source profile or add a 44.1k plug-in processing test.

A physically captured pointer can never gain the exclusion, even if its class or
the global pointer is replaced/reused. Missing, failed, null, unequal or wrong
owner observations and unknown classes continue through all existing strict
identity/format/gate guards and C9 diagnostics. Real stale Playlist reads still
enter their existing hold/rebuild path. No wider preview-pool rule is present.

`qualifiedPreviewUnrelatedCallbacks` is an additive **global router** observation
counter, incremented only by the positive narrow branch. It is not credited to
any insert and is not audio qualification. Its increment uses at most four CAS
attempts and saturates at uint64 max; contention can under-count. Existing
unrelated accounting also increments. No admission decision depends on either
counter.

The existing C9 diagnostic observed-POD mapping gains only
`globalPreviewOwnerReadCoherent` and `globalPreviewChannelObject`; existing field
names and finite/null/rawbits handling stay unchanged. These fields also permit
future rejected reads to show missing or mismatched default ownership.

Offline UBSan controls cover exact default ownership despite real clip/index
aliases, stale caches, preview zero-read preservation, failed/null/changing global
reads, same-class pool objects, unknown classes/voices, captured pointer reuse
within and across inserts, unchanged real Playlist/stale behavior, and counter
saturation. They are synthetic metadata/table controls, not a native runtime or
audio-output result. Current live default-global equality and actual C10 behavior
must be verified in the new immutable build.
