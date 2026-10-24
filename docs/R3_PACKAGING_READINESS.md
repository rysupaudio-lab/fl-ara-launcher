# R3 packaging readiness — source review only

R3 bridge modules have not been imported, built, signed or staged by this review.
Candidate2 remains immutable, unqualified and unsuitable for further save tests.
The owner must provide an exact source freeze and helper build before packaging.

## Startup and preservation

The launcher refuses pre-existing OsxFL applications before creating its session.
It starts only the exact owned private executable through its sandbox trampoline.
No process is adopted, hidden, signaled or terminated by the launcher.

An owned child exiting78 before native startup confirmation is recorded as
`startup_refused`. Its observed exit status and any exact-run/exact-PID
`owned_startup_save_guard_refused` or `owned_startup_engine_identity_refused` event
are retained. A missing event stays unknown. This result establishes no ARA
capability verdict. All startup/session logs remain in the owned run.

Cleanup permits removal only after the child exits and an exact-executable process
inventory reports no remaining private processes. A live child, duplicate private
process, or failed inventory preserves the app copy and lock. Cleanup never uses
an exit code alone as permission to remove a copy or touch another process.
Global default file-handler writes remain absent.

The native self-test now observes a real owned `/bin/sh` child exiting78, tests
conservative cleanup decisions, and rejects refusal events from other runs/PIDs.
It also exercises real child sandbox protection, isolated XML settings, executable
inventory and read-only handler decisions. No DAW, plugin or audio is launched.
Six Python tests cover build admission, exact source freeze and public manifest
redaction. No R3 compatibility qualification follows from these tests.

## Source and payload allowlists

After root freezes R3, reconcile only its sealed authored `.h`, `.hpp`, `.cpp`,
`.mm` and build/check `.py` files under `native/bridge/{host,native,legacy}`.
Keep external ARA/VST3 SDK source trees as build-time inputs. Do not import vendor
DAW/plugin binaries, registry/activation files, project media or runtime captures.
Preserve full private compiler receipts outside the application on Razer.

The app payload is the native Swift executable, our sealed helper, its small
allowlisted JSON manifest, README, MIT license, third-party notices, exact public
ARA Apache2.0 and VST3 pluginterfaces MIT texts, and generated code-signature files.
No Python, compiler, SDK source, vendor application, plugin, audio or secret is
required or bundled at runtime. Public input hashes use repository-relative names.
The full compile manifest, machine paths, commands and stale binary aliases stay
outside the bundle. Signing updates the canonical helper SHA before app signing.

## Exact front doors after the freeze

The first two environment values below must be set to the exact owner-provided
R3 helper and separately authorized qualification-admission manifest. The latter
must pin the helper bytes, remain explicit about its qualification scope, and
leave every unverified qualification flag false. Never change the original
compiler manifest's unqualified state to manufacture a package admission.

```sh
./build.sh --helper "$RYSUP_R3_HELPER" --helper-manifest "$RYSUP_R3_ADMISSION" \
  --output '/Volumes/Razer SSD/Rys Up Audio Archive/Development Resources/ARA App Integration/2026-10-24/fl-build/full-candidate-3'
/usr/bin/python3 tools/package_app.py \
  --app '/Volumes/Razer SSD/Rys Up Audio Archive/Development Resources/ARA App Integration/2026-10-24/fl-build/full-candidate-3/FL ARA Launcher.app' \
  --output '/Volumes/Razer SSD/Rys Up Audio Archive/Development Resources/ARA App Integration/2026-10-24/fl-build/full-candidate-3/packages' \
  --identity 'Developer ID Application: Rys Up Audio LLC (2XMVYX3RL4)' \
  --name 'FL ARA Qualification Candidate 3' --stem 'FL-ARA-Qualification-Candidate-3-arm64'
```

Extract/verify the ZIP, read-only mount/verify the DMG, verify Developer ID and the
actual signed helper SHA, check all public fields/relative seals, then stage a new
M1 candidate folder. No reuse/overwrite of a live or frozen candidate is permitted.
Root controls DAW launches and normal-project qualification separately.
