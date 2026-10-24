# Registration read-through — configuration routing only

`native/bridge-support/RegistrationReadThrough.{h,mm}` provides a process-local,
read-only view of the owner's existing `HKCU/Software/Image-Line/Registrations`
subtree. It does not alter vendor registration validation or decide edition,
version, eligibility, account state or lifetime updates. Missing/malformed source
or failed initialization leaves original private-hive callbacks unchanged; that
may leave ordinary trial behavior. No licensed-mode claim follows from routing.

The initializer accepts the non-secret `RYSUP_FLARA_ORIGINAL_REGISTRY_PATH` path
from the launcher and the exact copied WinBridge path. Production builds require
the registry to be the real current user's original Image-Line reg.xml, a regular
non-symlink owner file without group/world write permission. Source reads are
bounded, read-only and checked for inode/size/mtime stability. XML external entities
and DOCTYPE are rejected. Only the permitted subtree is retained in the map.
Registration values never enter environment variables, logs, reports, IPC, a
private registry file, installer payload or the vendor's mutable registry object.

Only symbol pointers from the exact copied, SHA-pinned WinBridge are wrapped.
Calls outside the registration subtree continue to the original private registry.
Tracked ordinary parent handles support relative opens. Synthetic registration
handles support only reads; create/set/delete/load/replace/restore/save and the
custom four-argument RegWriteStringValue are denied. The global RegSaveAllChanges
continues flushing the private hive, which never receives the original values.

The actual vendor API differs from ordinary Win32 conventions. Independent static
inspection and a 22-check isolated library probe established: HKCU=2; W strings
are UTF-16 even on this wchar_t32 platform; reported REG_SZ size is four times
UTF-16 unit count while bytes copied are UTF-16 plus NUL; binary XML is hex pairs;
Close is a no-op and handles remain valid; enumerators return only names and
leave length/type/data outputs unchanged. Prefixes in value names are literal.
Unsupported types and malformed hex fail closed. The adapter intentionally
refuses undersized output buffers with MORE_DATA234, rather than reproducing
vendor buffer overflow/truncation behavior. No data or successful registration
record is fabricated.

Integration: initialize after exact owned-app admission but before enabling native
callbacks. Route real lookup results through `observeRegistrationSymbol` from
dlsym and, if used by the FPC resolver, CFBundleGetFunctionPointerForName. Verify
only counter/boolean diagnostics initially, then let FL's unchanged validator
establish the actual licensed state. Keep original registry writes sandboxed.

## Synthetic verification

The test-only macro `RYSUP_REGISTRY_ADAPTER_TESTING=1` admits a synthetic fixture
path instead of the real user's registry. It must never appear in a runtime
helper build. The actual WinBridge is loaded by a tiny native probe; no FL engine,
DAW, plugin, audio or licensing validator is executed. The child sandbox blocks
opening the real registry and writing outside its owned UUID run.

The final probe passes75 checks, exits0 with empty stderr, and verifies Unicode,
relative/case-insensitive paths, types/payloads/sizes, capacity refusals,
malformed/unsupported values, close/info/enumeration,11 denied mutators, private
read/write forwarding, unchanged synthetic source, absence of auth canaries from
private serialized XML, and actual denied real-registry open/outside write.

Evidence directory on Razer:
`.../ARA App Integration/2026-10-24/license-static/registry-adapter-75034efd-d5a9-495b-8a30-60f0ff2c5cfd`.
The separate vendor-only ABI records and static report remain in the same
`license-static` parent. These are synthetic interoperability checks; actual
licensed normal-app behavior remains a separate runtime qualification.
