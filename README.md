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
- Make a signature-preserving APFS clone under an owned cache run UUID. Cross-volume
  full copying is allowed only on the explicitly selected mounted Razer SSD after
  non-symlink-following source sizing proves the whole copy plus 5 GiB reserve fits.
  There is no automatic internal-disk full-copy fallback. Launch its executable directly with `posix_spawn`, never `open`.
- Keep all private home/preferences/user data paths within that owned run, reject
  symlink ancestors, and synthesize only the private Shared data registry setting.
  Do not copy the user's registry, license, or whole home. The integration helper
  must preserve legitimate read-only authorization without duplicating secrets.
- Bind helper admission to exact executable, parent PID, run UUID and directory.
  Test mode admits only the `Pro Tools Audio Bridge 2-A_UID` audio-device UID. Normal mode
  allows the user's chosen normal audio device.
- Select only an existing backed-up `.flp` project, passed as one absolute path
  argument. Current-engine static parser proof is recorded in the integration
  `fl/PROJECT_ARGUMENT.md`; no `--` or `--args` separator is added. Runtime opening
  must still be confirmed through the actual current native project and graph.
- Remove only this exact application copy after its child and every live process
  whose executable path equals the private executable are confirmed exited.
  Any unbridged duplicate causes a visible error; no duplicate is adopted or stopped.
  Preserve every live process, copy and lock if the launcher exits or startup fails.
  Reopen the launcher and use **Clean finished session** after quitting that copy.
- Record baseline/current `.flp` handlers and unregister only an owned application
  copy after every process using it exits. Never write a global default handler;
  preserve and record changes made during the session. Keep diagnostics under `~/Library/Caches/FLARALauncher/runs`.

A child-only `sandbox-exec` profile denies writes to the original FL application,
Image-Line preference directory and existing preference domain files, configured
original user-data directory, and installed plugin directories/backing bundles.
It permits ordinary selected project backups and private caches. Physical paths
are resolved with Darwin `realpath`. A private shell trampoline sets the sealed
helper path into `DYLD_INSERT_LIBRARIES` after entering the sandbox (SIP may strip
DYLD variables on the system sandbox executable), then execs the exact copied
OsxFL, retaining the launcher as parent. No original registry/license is copied.
Normal-app tests must independently verify preservation and compatibility.

ARA project saves use our component-state envelope to retain both the genuine
ARA archive and the plugin's ordinary state. **Reopen and save those projects
through this launcher.** Unpatched FL does not understand this extension and
may discard the ARA state when saving. Keep an untouched project backup.

The current test profile is 48 kHz resident PCM16 mono/stereo audio with no
stretching, source-rate conversion, plugin latency or tail. Use one ARA effect
per mixer insert, placed first in its effect chain. Source caches are prepared
while stopped. An ARA edit during playback can stop playback for rebuilding;
playback remains stopped for an explicit restart. First Play before preparation
is complete requires stopping and waiting for preparation. These limits are
separate from actual plugin compatibility, which still requires native testing.

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
public distribution are separate recorded states. Builds produce owner-local qualification packages. A qualified public release
still requires actual normal-app and third-party runtime evidence.

CLI: `--version`, `--check`, `--fl PATH`, `--cache PATH`, `--cleanup`,
`--test-device "Pro Tools Audio Bridge 2-A_UID"`, or one backed-up `.flp` path. No hidden
runtime automation is activated by the offline checks.

Use `--cache-root "/Volumes/Razer SSD/.../FLARALauncher"` or the absolute
`RYSUP_FLARA_CACHE_ROOT` environment variable to select an external cache. The
private normal-app registry seeds FL Studio26/Devices/Audio output Sample rate
(Type2) at48000. Only test mode adds Device name (Type2)
`4Pro Tools Audio Bridge 2-A`; normal mode leaves the user's device unspecified.
Neither branch changes global preferences or duplicates the original registry.
The launcher waits180seconds for exact-run/exact-PID `fl-ara-status.json`
`runtime_helper_admitted` + `native_engine_ready`; a late live process is preserved.
Build module caches and temporary files stay under the chosen `--output` directory.

macOS native file-panel navigation history may be shared through the system panel
service. DAW/audio registry isolation remains in place. Qualification tests capture
and guard restoration of known changes they caused; they preserve independent
user changes. FL-specific navigation keys have not yet been established.
