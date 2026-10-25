# FL native Save: exact file-open ABI and frame proof

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


2026-10-24. Read-only static evidence for the proposed Save guard. No app or
plug-in was launched, no runtime was attached, and no installed file, registry,
preference, audio device or project was changed. This is not a runtime Save test.

## Artifact and evidence

## Proven route, including the initial non-creating open

`419bb0` receives target X0, save type W1, context X2 and flags W3. It constructs
its target-or-target-plus-tilde local and calls `a63cb0` at `419c70`, with native
stream mode **W3 = 0xff00**. `a63cb0` calls `a63e10` at `a63d48`; that calls
`a28650` at `a63ed8`. This is the actual TBufferedFileStream to TFileStream route.

`a28650` tests native mode's high byte at `a28700..a2870c`. A nonzero high byte
calls `f6320` at `a2871c`; a zero high byte calls `f6170` at `a28734` instead.
Therefore `0xff00` takes **f6320**. It does not directly become POSIX flags.

`f6320` first calls `f5ff0` at `f6348`, passing native mode unchanged. In
`f5ff0`, the POSIX access bits are derived from native mode `& 3`: zero maps to
`0`, one to `1`, two to `2`. For native `0xff00`, this first open is therefore
**O_RDONLY (0)**, through `a137f0` called at `f609c`. `a137f0` supplies default
mode `0x1b6` and calls `a12960` at `a137fc`. The first open can fail for a
nonexistent target; on success the file is checked and later locked/closed.

Then `f6320` calls `f6270` at `f6380`. That function supplies **W1 = 0x602** at
`f62c4`, zero-extends the incoming creation rights to 16 bits into W2 at
`f62bc`, and calls `a12960` at `f62c8`. Per the actual local macOS SDK,
`0x602 = O_RDWR | O_CREAT | O_TRUNC`. **O_EXCL is not set.** This is the
actual creating/truncating open that must never reach libc before admission.

The native rights argument comes from signed 32-bit global `1c58d18`, loaded by
`a63cb0` at `a63d2c..a63d34`, passed through W4 into `a28650`, then W2 into
`f6320` and W1 into `f6270`. Its sealed file initializer is **438 / 0x1b6 /
0666**, but that global is writable runtime data. Static evidence proves the
initial value and transfer, not that no runtime path changes it. A strict guard
must either verify the current value or accept/preserve the genuine current
16-bit mode; it must not pretend the file initializer is runtime readback.
The process umask can further restrict actual created permissions.

Native `f6170` (non-create mode) also calls `f5ff0`, then `f5eb0`; it does not
belong to the observed `0xff00` project-save create branch. Its return site in
`a28650` is `a28738`, distinguishable from create return `a28720`.

## Imported open and Darwin ARM64 variadic mode

`a12960` is the final native adapter:

```text
a12960 stp x29,x30,[sp,#-0x10]!
a12964 mov x29,sp
a12968 sub sp,sp,#0x10
a1296c str w2,[sp]
a12970 bl 0x1143060
a12974 mov sp,x29
```

Thus at the actual imported `open` entry, X0 is the native converted C pathname,
W1 is the actual POSIX flags, and the variadic `mode` promoted to `int` is at
**[SP]**. It is **not** consumed as a fixed third register argument by the
Darwin variadic ABI. An ordinary correctly declared C/C++ replacement
`int open(const char*, int, ...)` must read `va_arg` only when the applicable
creation flags require a mode, and forward it using an actual variadic libc
call. An assembly veneer must preserve this exact ABI rather than inventing an
X2-only function signature.

The actual stub is `1143060..1143068`: ADRP to `1b1c000`, LDR from
`1b1c688`, then BR X16. Both the stub's indirect-symbol entry and that lazy
pointer resolve to nlist symbol **`_open`**, index 261. The hook's direct
engine return PC is therefore **`a12974`**.

An aligned direct-BL scan of the sealed `__text` found two direct callers of
that stub: `a12970` above and `aa10`. The separate `aa00` adapter also spills
W2 to [SP] but is not on this observed native Save chain; its hook return PC is
`aa14`. Do not classify every engine-originating `open` as a project Save.

The sealed symbol table contains `_open` and `_dlsym`, and **no exact
`_open$NOCANCEL` import**. Exact NUL-terminated `open$NOCANCEL` and
`_open$NOCANCEL` byte searches over this complete sealed slice find no match.
That establishes absence of this named import and literal in this artifact;
it cannot exclude dynamically constructed names, other loaded images, libc
internals or future versions. Nothing here proves a process-wide guarantee
that no other route can use a cancellation variant.

## Exact frame chain to seal at the hook

The stub has no frame. For each real engine frame below, `[FP]` is its saved
parent FP and `[FP+8]` its saved parent return PC. Validate each frame's memory
range/alignment and image-bound instruction identity before interpreting locals.
Do not walk arbitrary unbounded frames.

**Creating/truncating open route:**

| Frame function | Saved return PC into parent |
|---|---:|
| `a12960` | `f62cc` (from `f6270` BL `f62c8`) |
| `f6270` | `f6384` (from `f6320` BL `f6380`) |
| `f6320` | `a28720` (from `a28650` BL `a2871c`) |
| `a28650` | `a63edc` (from `a63e10` BL `a63ed8`) |
| `a63e10` | `a63d4c` (from `a63cb0` BL `a63d48`) |
| `a63cb0` | `419c74` (from `419bb0` BL `419c70`) |
| `419bb0` | caller-specific; **not fixed by this route** |

**Initial read-only open route:** hook return PC remains `a12974`, but the
first three frame relationships differ:

| Frame function | Saved return PC into parent |
|---|---:|
| `a12960` | `a13800` (from `a137f0` BL `a137fc`) |
| `a137f0` | `f60a0` (from `f5ff0` BL `f609c`) |
| `f5ff0` | `f634c` (from `f6320` BL `f6348`) |
| `f6320` | `a28720` |

After that, the common suffix is `a28650 → a63e10 → a63cb0 → 419bb0` as
above. If preflight must run before **any** Save file IO, this initial
read-only route also needs recognition. At a minimum, the guard cannot treat
its O_RDONLY open as the later admitted O_CREAT transaction.

## Save frame locals: exact offsets

The Save function prologue is `419bb0 stp x29,x30,[sp,#-0x10]!`,
`419bb4 mov x29,sp`, `419bb8 sub sp,sp,#0x2a0`. Let **L = SaveFP - 0x2a0**.
The prologue and later reads prove:

| Native local | Address relative to Save FP | Instruction proof |
|---|---:|---|
| original target string value | `FP - 0x2a0` (`L+0`) | `419bbc str x0,[sp]` |
| save type u32 | `FP - 0x298` (`L+8`) | `419bc0 str w1,[sp,#8]` |
| context pointer | `FP - 0x290` (`L+0x10`) | `419bc4 str x2,[sp,#0x10]` |
| flags **u8** | `FP - 0x288` (`L+0x18`) | `419bc8 strb w3,[sp,#0x18]` |
| stream pointer | `FP - 0x278` (`L+0x28`) | `419c74 str x0,[sp,#0x28]` |
| temporary/direct target string value | `FP - 0x270` (`L+0x30`) | `419bd4`, `419c0c..419c30`, `419c58` |

The string slots hold managed native string **values/pointers**, not inline C
pathnames. The temporary slot is initialized null, then assigned original plus
native suffix `~` when `(flags & 1)==0`; otherwise it copies the original
value. The stream constructor receives that local's value in X2 at `419c58`.
The flags slot is one byte, not W3's original full 32 bits. A guard must validate
native string storage and conversions separately before comparing paths; merely
casting either frame local to `char*` is invalid.

## Lock flags and scope limits

`f5eb0` is the native sharing/lock adapter. For native `0xff00`,
`nativeMode & 0x70 == 0`, so it chooses **6 = LOCK_EX | LOCK_NB** at
`f5f1c`, then calls the imported **flock**, not fcntl, at `f5f64` via stub
`1142e80`. `f6320` applies it to the initial existing descriptor before close,
and to the new descriptor after the creating/truncating open. No lock protects
against truncation if guard admission is delayed until after `open` returns.

This proof identifies the exact current `419bb0` constructor/open route and
preflight timing. It does not prove all Save/autosave entry points, all project
commit/delete routes, protection from symlink races, custom filesystem behavior,
or success/failure propagation at runtime. Those remain implementation/runtime
qualification requirements. Existing `SAVE_FAILURE_SAFETY.md` still applies:
component getState refusal alone is not a native project Save veto.
