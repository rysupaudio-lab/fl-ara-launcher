# FL ARA Launcher — native candidate source

An independent, experimental Apple Silicon macOS launcher for a **private copy**
of the owner's licensed FL Studio 2026 **26.1.7.5421**. This source is under active
normal-app integration. It is not yet a qualified public ARA patch.

The Swift/AppKit application needs no Python or developer tools at runtime. Build
and packaging tools use Python and Xcode command-line tools on the build machine.
The app contains only the launcher, our native helper, documentation, and license;
no Image-Line application files, third-party plugins, or license keys are shipped.

## Preservation contract

- Verify the exact OsxFL and FLEngine SHA-256 pins and original Image-Line Team
  N68WEP5ZZZ signature, before and after each session.
- Refuse while another OsxFL application is running. Never attach, hide, adopt,
  signal, or terminate a pre-existing/user FL instance.
- Make a signature-preserving APFS clone under an owned cache run UUID. Fail if
  cloning fails. Launch its executable directly with `posix_spawn`, never `open`.
- Keep all private home/preferences/user data paths within that owned run, reject
  symlink ancestors, and synthesize only the private Shared data registry setting.
  Do not copy the user's registry, license, or whole home. The integration helper
  must preserve legitimate read-only authorization without duplicating secrets.
- Bind helper admission to exact executable, parent PID, run UUID and directory.
  Test mode admits only the `ProToolsAudioBridge2A` audio-device UID. Normal mode
  allows the user's chosen normal audio device.
- Select only an existing backed-up `.flp` project. Native project-open argv is
  awaiting the standalone-entry owner's qualification. Do not assert this route
  qualified on the basis of compilation.
- Remove only this exact application copy after its child is confirmed exited.
  Preserve a live process, copy and lock if the launcher exits or startup fails.
  Reopen the launcher and use **Clean finished session** after quitting that copy.
- Snapshot/restore the `.flp` default handler and unregister private application
  copies. Keep diagnostics under `~/Library/Caches/FLARALauncher/runs`.

A child-only `sandbox-exec` profile denies writes to the original FL application,
Image-Line preference directory and existing preference domain files, configured
original user-data directory, and installed plugin directories/backing bundles.
It permits ordinary selected project backups and private caches. Physical paths
are resolved with Darwin `realpath`. A private shell trampoline sets the sealed
helper path into `DYLD_INSERT_LIBRARIES` after entering the sandbox (SIP may strip
DYLD variables on the system sandbox executable), then execs the exact copied
OsxFL, retaining the launcher as parent. No original registry/license is copied.
Normal-app tests must independently verify preservation and compatibility.

## Build without launching FL

```sh
./build.sh --compile-only
'dist/FL-ARA-Launcher-compilecheck' --self-test
'dist/FL-ARA-Launcher-compilecheck' --check
```

The self-test uses only disposable directories and never launches FL. `--check`
reads the installed application and verifies vendor signatures and exact hashes.

## Build the application with the qualified native helper

```sh
./build.sh --helper /path/to/FLAppBridge.dylib --helper-manifest /path/to/helper.json
python3 tools/package_app.py --identity 'Developer ID Application: …'
```

The helper manifest must be issued by the integration owner, have schema
`rysup.fl-ara-helper/1`, `runtime_ready: true`, `binary: FLAppBridge.dylib`, and a
`sha256` matching the exact signed helper bytes. Missing, unready, or placeholder
helpers cannot be packaged or launched. The packager refreshes the binary seal
after signing the helper and before signing the application.

Packaging produces a drag-to-Applications DMG, ZIP and SHA-256 receipt. Signing,
notarization, normal-app testing, third-party compatibility qualification and
public distribution are separate recorded states. This source currently provides
no finished installable package; the native helper remains integration-owned.

CLI: `--version`, `--check`, `--fl PATH`, `--cache PATH`, `--cleanup`,
`--test-device ProToolsAudioBridge2A`, or one backed-up `.flp` path. No hidden
runtime automation is activated by the offline checks.
