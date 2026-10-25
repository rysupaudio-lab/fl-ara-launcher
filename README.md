# FL ARA Launcher

An experimental ARA 2 integration for **FL Studio 2026 26.1.7.5421 on Apple Silicon Macs**. It runs your licensed FL Studio in a private application copy and keeps your installed FL application unchanged.

## Install

1. Download the **arm64 DMG** from this repository’s Releases page.
2. Open it and drag **FL ARA Launcher** to **Applications**.
3. Open **FL ARA Launcher** and choose **Open a backed-up project…**.
4. Choose your licensed FL Studio application if prompted. Work in the private FL window and keep the launcher open.
5. Save your project, then quit that FL window normally when finished.

No Terminal commands, Python or developer tools are needed. Your ARA plugins must be installed and licensed separately. No Image-Line application files, plugin binaries or license keys are included.

**Projects containing this integration’s ARA edits must be reopened and saved through FL ARA Launcher.** Ordinary unpatched FL does not understand its extended component state and may discard ARA edits or plugin settings when saving. Keep an untouched project backup.

## Compatibility

| Requirement | Current scope |
|---|---|
| Mac | Apple Silicon / arm64 |
| FL Studio | Original Image-Line-signed **26.1.7.5421**; other builds are refused |
| macOS | App deployment minimum **15.4**; native evidence so far is from **macOS 27.0 (26A428)** |
| Audio | **48 kHz**, resident PCM16 mono/stereo source audio |
| Insert placement | One ARA effect per mixer insert, first in the effect chain |
| Source processing | No source stretching or source-rate conversion; no plugin latency or tail in this initial route |
| Plugin evidence | Real autochoir 1.0.0 VST3 integration work; compatibility is plugin-specific |

Candidate **0.1.0-candidate.12** contains the native-save, slot-bypass and ready-gated export integration. Its release notes must identify the exact completed runtime evidence; source compilation alone is not audio or recall qualification. This is an experimental release, not general ARA conformance or support for every plugin, DAW version or Mac.

Source caches prepare while playback is stopped. An ARA edit during playback can stop playback for preparation; restart playback explicitly after preparation completes.

## Your installed FL stays intact

The launcher verifies the original Image-Line signature and exact application hashes, creates its own temporary app/home/preferences, and denies writes to the installed DAW, installed plugins and normal FL preferences/user-data directories. Legitimate authorization is read without copying license data.

Other installed DAW sessions are left alone. A failed or interrupted launcher preserves its private session so you can save your work. After that private FL process exits normally, **Clean finished session** removes its finished copy. Diagnostics stay under `~/Library/Caches/FLARALauncher/runs`.

The private copy normally uses a same-volume APFS clone. If FL is on another volume, the launcher currently supports a full-copy fallback only on an explicitly selected mounted **Razer SSD**, after checking space. It has no automatic internal-disk full-copy fallback. Very long cache paths are refused because this FL build truncates native template paths; use the default cache or a short real path.

To uninstall, drag only **FL ARA Launcher.app** to Trash after its private sessions have exited. Your installed FL Studio remains in place.

## Build from source

Build tools require Apple Silicon, Python and Xcode command-line tools; the installed launcher does not.

```sh
./build.sh --compile-only
'dist/FL-ARA-Launcher-compilecheck' --self-test
'dist/FL-ARA-Launcher-compilecheck' --check
```

Build the app with the exact helper and its matching manifest:

```sh
./build.sh --helper /path/FLAppBridge.dylib --helper-manifest /path/helper.json --output /path/build
/opt/homebrew/bin/python3.13 tools/package_app.py \
  --app '/path/build/FL ARA Launcher.app' --output /path/packages \
  --identity 'Developer ID Application: …' \
  --version 0.1.0-candidate.12 --stem FL-ARA-Launcher-0.1.0-candidate.12-arm64
```

The helper must be signed and hash-bound to a ready build manifest. Signing, notarization, runtime evidence and publication are separately recorded. See [candidate release preparation](docs/CANDIDATE_12_RELEASE.md).

The internal CLI also accepts `--headless-export BACKED_UP.flp` with normal physical-device mode. It opens the same private normal FL application, waits for current ARA preparation and writes only `export.wav` plus a result receipt inside its new owned run. It never saves or quits automatically. A result file or native Boolean alone is not an audio-compatibility claim. The native export may create its own render form; this route requires no computer-use gestures.

## License

MIT for the launcher/integration code. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled SDK notices. Independent of Image-Line and plugin vendors.
