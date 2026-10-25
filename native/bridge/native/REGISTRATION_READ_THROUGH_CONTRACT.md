# Original registration read-through

The owned helper may read the original user's Image-Line registration subtree
into process memory. The original vendor validator decides license validity,
version and edition. Successful adapter initialization is not license validation.
No registration payload is copied into private preferences, environment variables,
diagnostics or candidate packages. Other preferences remain in the private profile.

Initialization requires the exact original per-user registry path and sealed
WinBridge from the owned application. It checks regular-file ownership and mode,
size, UTF-8/XML format, stable metadata and unambiguous registration nodes. It
rejects symlinks, unsafe XML and conflicting registration entries. If unavailable,
it leaves ordinary native behavior intact and reports the failure; it never
fabricates a licensed result. Registration write attempts remain denied.

The registration root string uses function-local static initialization because
the helper's constructor can run before namespace-scope C++ dynamic initializers.
The root must be available before the first parser callback, independent of
translation-unit link order. Initialization retains the existing validation rules.

Diagnostics expose only fixed numeric initialization stage, immediate errno,
bytes read, XML numeric error location, and node/value counts. Stage 45 means
adapter initialization completed. Status still sets `vendor_license_validity_proven`
to false. Failure events include only stage and errno. Paths, registration names,
identities and values must never be included in diagnostic payloads.

Offline constructor-order and ABI controls do not qualify actual application
startup, editing, saving, audio playback or third-party compatibility. Those
remain separate runtime checks against the exact packaged binary.
