# Routing-only qualification candidate

This candidate requires supported mixer-insert routing for every captured audio
channel during graph/model, native Save and offline export tests. Its sole
compiled change from the prior output-scope candidate sets
`requireSupportedMixerInsertForEveryAudioChannel=true`. Native application and
test-output sink implementations remain at the prior frozen versions.

It contains no device-start fix, retains the prior device-start guards, and
provides no audio I/O qualification. Compilation and offline controls do not
establish successful project loading, ARA editing, native Save, export or
third-party behavior. All actual runtime qualification flags remain false.

The signed helper manifest and package receipt carry `routing_only=true`,
`no_device_start_fix=true`, `no_audio_io_qualification=true`, and fixed scope
`graph-model-native-save-offline-export-testing-only`. Packaging rejects
contradictory claims and preserves these restrictions after signing. This
metadata describes the qualification scope; it does not add a new device gate.

Runtime tests use the exact owned application, private profile and backed-up
project. Independent unrelated DAW instances are allowed; exact-copy ownership,
duplicate-process preservation, write sandbox and cleanup checks remain active.
