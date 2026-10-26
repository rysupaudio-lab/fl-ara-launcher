# FL ARA Launcher

An experimental ARA 2 integration for **FL Studio 2026 26.1.7.5421 on Apple Silicon Macs**. It runs your licensed FL Studio in a private application copy and keeps your installed FL application unchanged.

**[Download the Mac installer (DMG)](https://github.com/rysupaudio-lab/fl-ara-launcher/releases/download/v0.1.0-candidate.13/FL-ARA-Launcher-0.1.0-candidate.13-arm64.dmg)** · [Release notes and checksums](https://github.com/rysupaudio-lab/fl-ara-launcher/releases/tag/v0.1.0-candidate.13)

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

Candidate **0.1.0-candidate.13** includes native project saving and the genuine FL batch-render path with ARA preparation after project loading. Native saves retain both ordinary plugin state and genuine ARA archives; paired autochoir Formant values of +5 were saved. Three independent cold headless runs have played all six full/cropped/repeated mono/stereo clips, produced identical Float32 stereo 48 kHz audio (276000 frames), and exited normally with no private FL processes left running.

**Audible editing is not working in the latest tested parameter A/B.** Using the installed candidate13 app, changing both autochoir Detune values from .03 to .05 produced exactly identical audio: zero changed samples. Genuine ARA binding/source playback and saved state do not establish working audible edits. Treat this as an unfinished research integration, not a working audio-editing patch. The unpatched control is a rendering reference, not a processed ARA reference; an audible Formant DSP change also remains unproven. Melodyne 5.4.2 previously bound, rendered its default six-clip project and saved on quit; its editing and cold recall are unqualified. This is an experimental integration, not general ARA conformance or compatibility with every plugin, DAW version or Mac.

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
  --version 0.1.0-candidate.13 --stem FL-ARA-Launcher-0.1.0-candidate.13-arm64
```

The helper must be signed and hash-bound to a ready build manifest. Signing, notarization, runtime evidence and publication are separately recorded. See [candidate release preparation](docs/CANDIDATE_13_RELEASE.md).

The internal CLI accepts `--headless-export BACKED_UP.flp` for a disposable, read-only-source batch run. It uses FL’s real `-R` batch renderer, prepares ARA after the genuine project loader returns, and reuses the sealed converter RateHold and export driver from the control pipeline. Output and private diagnostics stay in the owned run. The existing driver waits for the output and quits the private FL copy normally; it can answer discard prompts for that disposable batch only. No computer-use gestures are needed. The normal launcher window/project-saving workflow above is unchanged.

## License

MIT for the launcher/integration code. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled SDK notices. Independent of Image-Line and plugin vendors.
