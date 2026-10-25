# Native full-project Save guard

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


## Why the guard exists

The shipped Fruity Wrapper skips its component-state chunk when getState fails,
then continues serializing other state. It does not turn the refusal into a
whole-project Save failure. A bridge returning failure while playing is therefore
insufficient to preserve ARA state. See `../SAVE_FAILURE_SAFETY.md`.

`FLNativeSaveGuard.mm` intercepts the actual imported variadic `open` before the
native creating/truncating call. The exact current native constructor chain is
`419bb0 → a63cb0 → a63e10 → a28650 → f6320 → f6270 → a12960 → _open`.
The actual flags are `O_RDWR|O_CREAT|O_TRUNC` (`0x602`); the preceding read-only
open forwards. The original variadic mode is preserved. Actual native Save stack
frames identify transactions; filename extensions do not authorize them.

The guard validates the admitted private process, owning engine path, bounded
contiguous frame chain, stack range, loaded instruction pins, FPC UTF-16 header /
body and repeated native local reads. Exact full-file hashes are supplied by
`FLNativeApplication` admission. The native constructor chain fails closed even
if its pathname conversion unexpectedly differs. Incomplete stack walks refuse
write/destructive effects while ARA or a guarded transaction is active.

## Owner integration contract

Initialize on the admitted main thread with callbacks in this order:
`preflight, precommit, rejected, finished, context` from the owner's
`beforeOriginalCreate` callback, after the actual dlsym engine binding exists.
Do not initialize from the earlier `admitted` callback: the image binding can
still be empty there. Initialization and readiness both check all 35 loaded-code
pins, the engine base / exact loaded image path and admitted ARM64 slice hash.
They require neither `engineReady` nor `engineInstance`, and never refresh or
invoke native engine state during startup. Require
`nativeSaveGuardReady()` before enabling ARA. Set `callbacks.hasLiveARA` to a
fresh owner predicate that reads only stable atomic native-record flags and
lifetimes, including incomplete tracking. It must not call SDK/file APIs, allocate
or lock. The guard evaluates it at the actual file boundary; an earlier cached
main-thread false value cannot erase a background discovery. The callback table
is immutable and release/acquire published after successful guard initialization.
`setNativeSaveGuardHasLiveARA` remains only a fallback when no predicate is
supplied; its cached value is ignored when the owner predicate exists. Pending
transactions remain protected regardless of the current predicate. The helper
must be built with frame pointers; production compilation contains no synthetic
frame API. Add its `.mm` to the private helper build so its `__DATA,__interpose`
entries are linked. No host instruction patch is used.

Preflight freezes already-prepared strict archives for every expected live ARA
component. It may use only verified owned HostLockMix and bounded read-only
engine/source seals and native RAM capture/decode under stopped, process-drained
admission. It must not invoke ARA, a plug-in/state getter, another native API or a
file write. A false result sets `EBUSY` before native file creation. Any returned
cookie is released through `finished`, including refusals / original-open errors.

Actual main-thread native getState calls use
`currentNativeSaveTransaction()` to identify the exact frame, serial and frozen
owner cookie. Every refusal must call `markCurrentNativeSaveFailed()`; every
expected component's successfully emitted envelope must be recorded by the owner.
No expected component may be silently omitted, serialized off-main or replaced
with an ordinary payload.

Before the first destructive unlink or Foundation move, `precommit` must prove
that every expected component emitted its frozen valid envelope and that all
frozen tickets, invalidation epochs and current RAM still agree. It has the same
read-only restrictions as preflight. Missing or false precommit refuses before
the old target changes. The guard covers actual native replacement and native
temporary-file cleanup frames; unrelated file operations otherwise forward.

RT/background changes call `markActiveNativeSaveFailedFromAnyThread()`. This uses
only a constant-initialized lock-free atomic pointer and phase CAS; stable nodes
remain allocated until process exit so an in-flight RT reader cannot dereference
a freed transaction. The optimized object contains no call instruction in this
entry. It cannot perform stack walks, allocations, locks, callbacks or SDK work.

`hasActiveNativeSaveFromAnyThread()` is a separate atomic-only snapshot. It is
true for a published failed/precommit or committed transaction until its actual
frame is retired, and false without a node or after finishedBit is set. It reads
only the constant-initialized active pointer and node phase; its optimized body
also contains no call. The owner can snapshot around genuine/protected flag
publication and invalidation, queueing a native dirty revision only when an
actual save was active. Ordinary cold-load discovery must not become dirty just
because no transaction was available to invalidate.

The first destructive effect linearizes commit against background invalidation.
If invalidation wins, the old project and temp remain untouched. If commit wins,
later background dirtiness cannot abort the remaining move after the old target
has already moved aside. Ordinary native file-IO errors retain native semantics.

For predicate-backed ordinary Saves, a false owner predicate permits normal temp
creation without an ARA freeze. If ARA appears before the first destructive
effect, the missing prepared transaction vetoes that effect and preserves old
and temp bytes. If the predicate is still false at the first proven native
destructive effect, the guard tracks an explicit `ordinaryCommit` transaction:
no preflight/precommit, no archive or cookie is manufactured. It publishes the
active node, rechecks current ownership, then uses the same commit CAS. Later
ARA arrival cannot interrupt the remaining native move and strand an already
removed target. The owner must publish genuine-ARA / protected-envelope flags
with active-transaction invalidation; if commit already began, the new revision
must remain unsaved/dirty. This ordinary tracker claims no ARA serialization.

The owner's main tick calls `pollNativeSaveGuardTransactions()`. Only after the
exact native Save frame disappears does `finished` run once and release the
freeze. It performs no file cleanup. Same-frame repeated create/truncate and
nested Saves refuse rather than reuse a freeze. A synchronous new Save before
the main maintenance tick can conservatively refuse if it reuses the old FP.

## Evidence and remaining qualification

These tests substitute native admission and frame discovery. They do not prove
the real normal-FL Save UI, actual frame walking, owner frozen-archive integration,
native error presentation, every autosave/export command, symlink races, custom
filesystems or all possible unobserved commit paths. Qualification must use only
owned copies and projects. This is not an installable-patch or customer-release
claim.
