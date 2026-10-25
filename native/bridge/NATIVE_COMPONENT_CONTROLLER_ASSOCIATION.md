# Native component / controller association

Public implementation contract. Exact normal-app and third-party runtime qualification is pending; no release claim is made. Private laboratory reports and historical test totals are not part of this source distribution.


2026-10-24 static evidence only. No DAW, plug-in, SDK factory or loaded module was
executed. SaveGuard and NativeApplication source were not changed for this work.

The installed universal Fruity Wrapper hash was rechecked as
`445dd1766e95013acc5046d7f08346fa04ef451a5d16e035a800518199c4d456`.
Its immutable ARM64 copy is
`7ce89557800749bd30f525ebd316f8d47d5d1c5ff4eb3c38068d7a3972d61190`.
All addresses below are unslid wrapper-relative VM addresses.

The actual native `TVST3Plugin` object P (VMT `2afb80`) stores the returned
`IComponent*` at **P+818** and returned `IEditController*` at **P+828**. These are
native pointer fields, not inferred class IDs, creation order or payload hashes.

## Exact host and initialization route

P's constructor passes its own pointer as X2 to `8c310` at `7fa00`, requests a
new `TVST3HostInstance` class (`2b0010`), and stores returned H at P+7a8 (`7fa08`).
The H constructor stores the original X2 owner at **H+8** (`8c390..8c394`). This
provides the native reciprocal checks H+8=P and P+7a8=H. The existing typed
initialize context is **H+18**, with interface vtable `2b3dc8`.

The component factory call `838a0` receives `&P[818]` as its output. The wrapper
then initializes that exact component with X0=P[818], X1=H+18 via vtable slot18
at `83928`, so the actual native caller return PC is **8392c**.

Next the wrapper queries the actual component for the controller interface at
`83984`, with output directly `&P[828]`. A successful nonnull result takes the
combined-instance route and does **not** perform a separate controller initialize.
The existing hook's same-record QI observation remains the association for this
case; a new initialize callback must not be invented or required.

If QI does not supply a controller, the wrapper calls the actual component's
getControllerClassId at `839cc`, then invokes its genuine factory at `83a70`,
again with output directly `&P[828]`. The separate controller is initialized with
X0=P[828], **the same X1=H+18**, via vtable slot18 at `83ab4`. Its exact native
initialize caller return PC is **83ab8**. No controller QI back to IComponent or
shared FUnknown identity is guaranteed for this separate object.

## Association and revalidation contract

For a component callback, use its observed actual initialize context to recover
H=context-18. Verify the complete existing typed reciprocal H/P/owner chain and
require **P[818] == actual component this**. The earlier resolver accepted the
argument without this final field equality; that check must be added.

For a separately created controller callback, recover the same typed reciprocal
chain from its actual observed initialize context, require
**P[828] == actual controller this**, and read **P[818]** to locate the already
observed native component owner. Repeat both pointer reads and full reciprocal
validation at every actual state call. An initialize context alone is not enough
to accept a stale/replaced controller: current native fields and observed
interface publication / lifetime must still agree.

Keep the component and separately created controller's InstanceRecords and
native lifetimes distinct. Link controller operations to the component owner
through these current native fields. Do not merge release/terminate accounting,
perform an extra QI to guess identity, or pair by class ID / stream contents.
Current hooked vtable publications belong to the observer; validate those using
its existing exact publication records rather than requiring an original table
pointer after hooks have legitimately installed a private table.

The ordinary cold restore calls component.setState via P[818] at `88a94`, then,
for a separate controller, controller.setComponentState via **P[828]** at
`88afc` (return **88b00**). Initial separate-controller synchronization calls
the same controller vtable slot28 at `83d30` (return **83d34**). Both provide
direct source proof for choosing the controller field on these operations.

The actual Save path independently loads component P[818] at `87fa8/87fc0` and
calls getState vtable68 at `87fcc`; controller P[828] is loaded at
`88244/8825c` and its getState vtable38 called at `88268`. The native controller
component handler is **P[7b0]+18** at `83c1c`, distinct from the initialize
context **P[7a8]+18**. Do not use a handler pointer as the initialize owner link.
`native/FL_WRAPPER_POINTER_ASSOCIATION.md` adds SDK-slot verification and optional
IConnectionPoint observations. Connection interfaces/results are optional and
are not needed for the actual pointer-field association above.

## Retained evidence
