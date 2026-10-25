# Sealed FruityWrapper: actual component/controller pointer association

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


2026-10-24, static only. No dlopen, DAW or plug-in launch, process attachment,
installation change, native import or Save-guard source edit occurred.

## Actual native fields

For a separately established actual TVST3Plugin object **P** (VMT `2afb80`):

| Field | Native meaning | Exact instruction witness |
|---|---|---|
| `P+818` | actual IComponent interface pointer | factory output address at `83884`, used for initialize at `83914..83928` and component getState at `87fa8..87fcc` |
| `P+828` | actual IEditController interface pointer | component QI output address at `83968`, or separate factory output at `83a54`; used for controller getState at `88244..88268` |
| `P+810` u8 | native combined-component/controller ownership flag | set 1 at `83950`; cleared before separate-controller fallback at `839ac` |
| `P+848` | component's queried IConnectionPoint | QI of `P+818`, output address `8764c`, actual call `87660` |
| `P+850` | controller's queried IConnectionPoint | QI of `P+828`, output address `87728`, actual call `8773c` |

These are actual returned interface pointers stored directly by the called
factory/QI. They are not identities inferred from class IDs, creation order,
module hashes, product strings or vtable equality.

`P+808` is a native factory/module holder; the actual IPluginFactory pointer is
at `[P+808]+8`. The native factory's vtable+30 is standard createInstance.

## Actual component output

`83854..8385c` obtains the standard IComponent IID from native GUID source
`340c28` through conversion/cache helper `255450`. At `83874..838a0`, the
wrapper calls:

```cpp
factory->createInstance(actualClassID, actualIComponentIID, &P.component818);
```

Registers at BLR `838a0`: X0 is actual factory interface, X1 is class ID bytes,
X2 is IComponent IID bytes, X3 is **P+818**. It checks result zero and nonnull
`P+818` at `838a4..838c0` before proceeding. Native initialize then uses this
same stored pointer, calling vtable+18 at `83928`, with native host context
`[P+7a8]+18` (when nonnull) in X1.

The source GUID bytes at `340c28` are
`31ff31e8d5f20143928ebbee25697802`, matching SDK IComponent IID
`E831FF31-F2D5-4301-928E-BBEE25697802` in the wrapper's native GUID storage.
The helper converts/caches that source representation into actual IID bytes;
do not compare the raw 16-byte native GUID source directly to an arbitrary
plug-in's ABI TUID without following that representation conversion.

## Controller QI and separate-controller fallback

At `83954..83984`, IID source `340cd8` is converted using `255450`, then the
wrapper invokes actual `P+818` vtable+0:

```cpp
P.component818->queryInterface(actualIEditControllerIID, &P.controller828);
```

Registers at BLR `83984`: X0 is actual `P+818`, X1 is actual IEditController IID,
X2 is **P+828**. Result zero plus nonnull stored pointer at `83988..839a4`
bypasses the separate-controller creation/initialization block and retains
`P+810=1`. The controller interface can have a different numerical pointer
from IComponent even when it belongs to the same underlying combined object.

On failure/null output, native code clears `P+810` at `839ac`, calls actual
component getControllerClassId via vtable+28 at `839cc`, and, if supported,
invokes the actual factory createInstance at `83a70` with output address
**P+828**. The CID selects the requested class but is not the association proof:
the concrete pointer written into **this exact P field** is the relevant link.
A separately created nonnull controller is initialized through vtable+18 at
`83ab4`, with the same native host context `[P+7a8]+18`. Initialization failure
releases actual `P+828` via vtable+10 at `83b18`, then clears the field at
`83b20`.

Source GUID `340cd8` raw bytes are
`e3bbd7dc42778d44a874aacc979c759e`, matching SDK IEditController IID
`DCD7BBE3-7742-448D-A874-AACC979C759E` in native GUID storage.

## Actual reciprocal component/controller communication

After the acquisition above, `83c24` calls native helper `87610(P)`. That
queries IConnectionPoint separately from actual component and actual controller,
storing the results in `P+848` and `P+850`. The common IID source is `340c98`,
raw bytes `6f15a4706e6e2640989148bfaa60d8d1`, matching SDK IConnectionPoint
`70A4156F-6E6E-4026-9891-48BFAA60D8D1` in native GUID storage.

If both connection pointers are nonnull and `P+810==0`, actual native code
performs **both directions**, using the real stored pointers:

- `87840..87854`: `P.componentCP848->connect(P.controllerCP850)` via vtable+18.
- Only if the first return is zero, `87860..87874`:
  `P.controllerCP850->connect(P.componentCP848)` via vtable+18.

The second result is not checked in the observed helper. The static calls do
not prove successful runtime connection. Native teardown calls disconnect in
both directions at `8a4f4` and `8a50c` via vtable+20 before releasing the stored
interfaces. Missing IConnectionPoint is allowed by the actual native code.
Do not make its availability or success a universal plug-in assumption.

For controller host callbacks, `83bf4..83c1c` passes `[P+7b0]+18` to actual
`P+828->setComponentHandler` at vtable+80. This host handler is **different from
[P+7a8]+18**, the initialize context. Its internal reciprocal P backlink is
outside this focused component/controller task and must be independently pinned
before using it as authority.

## Vtable slots and concrete state witnesses

SDK inheritance is FUnknown (`queryInterface +0`, `addRef +8`, `release +10`),
then IPluginBase (`initialize +18`, `terminate +20`). Offsets below are hex.

| Interface | Slot | Method | Actual wrapper use |
|---|---:|---|---|
| IComponent | `28` | getControllerClassId | `839cc` |
| IComponent | `58` | setActive | `8a410` |
| IComponent | `60` | setState | `840ac`, `84ba4` |
| IComponent | `68` | getState | `87fcc`, `83ce4` |
| IEditController | `28` | setComponentState | `83d30`, `84108`, `84c38` |
| IEditController | `30` | setState | `84e34` |
| IEditController | `38` | getState | `88268` |
| IEditController | `40` | getParameterCount | `878c8` |
| IEditController | `80` | setComponentHandler | `83c1c` |
| IConnectionPoint | `18` | connect | `87854`, `87874` |
| IConnectionPoint | `20` | disconnect | `8a4f4`, `8a50c` |

At native project component serialization, `87fa4..87fcc` loads P from the
serializer local, then **P+818** into both actual this and vtable lookup; this is
the actual component that owns the native state call. Controller serialization
`88240..88268` does the corresponding **P+828** loads. Those call-site witnesses
can independently verify association at interception time without matching a
class ID or relying on initial creation order.

## What a two-way runtime association may safely establish

A runtime record should bind the actual intercepted interface pointer to the
exact currently read `P+818` or `P+828`, and also validate the independently
source-proven reciprocal P↔native-host/context relationship. Double-read P's
field and the reciprocal backlink around admission, with exact current VMT,
image and loaded instruction seals; reject mismatches and ambiguous ownership.
Native state call-site witnesses provide additional direct confirmation.

Do **not** require `P+818==P+828`, assume controller can QI IComponent, use
controller getControllerClassId (it has no such SDK method), or require a shared
canonical FUnknown for separate component/controller objects. Static analysis
shows both combined and separately created paths. There is no general standard
reverse QI mapping from arbitrary separate IEditController to IComponent.

This report proves where the actual pointers come from and how native code uses
them. It does not provide runtime pointer readback, prove successful third-party
initialization, prove an arbitrary plug-in's internal owner pointers, or replace
parent's separate native host/context/backlink proof.
