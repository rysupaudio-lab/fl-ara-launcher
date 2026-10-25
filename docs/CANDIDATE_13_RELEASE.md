# Candidate 13 release notes

Prepared version: `0.1.0-candidate.13`. This is a bounded experimental integration. Signing, notarization and publication must be reported from their actual receipts, not inferred from this document.

## Install

Download the arm64 DMG, open it and drag **FL ARA Launcher** to **Applications**. Open the launcher and choose **Open a backed-up project…**. Use its private FL window and keep the launcher running. Save normally and quit that private FL window when finished. No Terminal commands or developer tools are required for normal use.

Requires your original Image-Line-signed **FL Studio 2026 26.1.7.5421**, an **Apple Silicon Mac**, and separately installed/licensed ARA plugins. The app deployment minimum is **macOS 15.4**; native results currently come from **macOS 27.0 (26A428)**. Other FL builds, Intel Macs and Windows are outside this release’s scope.

Always reopen and save projects containing this integration’s ARA state through the launcher. Ordinary unpatched FL does not understand its extended component state and may discard plugin settings or ARA edits when saving. Keep an untouched project backup.

## Changes and current evidence

Candidate 13 uses FL’s genuine native command-line batch renderer and prepares current ARA caches after the genuine project loader returns. It reuses the existing converter’s RateHold and export driver, including output waiting and normal quit, rather than calling the render form directly. Normal interactive launcher behavior is unchanged.

Native project Save has retained two genuine RYSFLAPP envelopes containing ordinary plugin state, genuine ARA archives and paired autochoir Formant values of +5. Three independent cold headless runs each contain all six full/cropped/repeated mono/stereo clips, Float32 stereo at 48 kHz and 276000 frames. All three exited normally with code 0 and no private FL processes left running. Their WAV files are byte-identical: SHA256 `bb4dbd79580ddfd86e097e6be1eaaca7e09f1670d0d199c4a040cda2ec998047`. The input and original FL application remained unchanged.

**Three-run cold acceptance is complete for this exact experimental profile.** Runtime evidence is from the r13c pipeline; the code revision tested was `e734d6d`. This covers the stated clip/state/render path and normal cleanup, not general ARA conformance.

The unpatched control establishes expected clip coverage and render geometry; it is not a processed ARA audio reference. Current processed/control differences are limited to subnormal values and do **not** establish an audible Formant DSP change. Saved +5 parameter values and opaque archives prove state retention, not audible correction.

Melodyne 5.4.2 previously bound through the genuine ARA path, rendered its default six-clip project and completed native Save on Quit. Melodyne editing and cold recall remain unqualified. No universal plugin compatibility or ARA conformance claim is made.

Initial audio scope remains 48 kHz resident PCM16 mono/stereo source audio, one ARA effect first in each mixer insert, with no source stretching/rate conversion or plugin latency/tail in this route. Float32 describes the native batch output, not an expansion of supported source encodings.

## Internal headless route

`--headless-export BACKED_UP.flp` uses a disposable, read-only-source batch. The launcher injects only its sealed bridge, RateHold and the exact `14fe69aa` converter export driver in this mode. Its private request directory belongs to that launcher process. The existing driver waits for output and requests normal Quit, with narrow answer rules for the owned disposable batch. It does not operate other DAW sessions. Normal GUI launches do not inject those batch companions.

The source/compiled helper and companion hashes, native process result, WAV header/hash and installed-application preservation are separate evidence. Output presence or a native Boolean is not sufficient. Diagnostics and audio fixtures are private and are not release assets.

## Packaging

Use `tools/build_app.py` / `build.sh`, then `tools/package_app.py` with explicit version `0.1.0-candidate.13` and stem `FL-ARA-Launcher-0.1.0-candidate.13-arm64`. Nested bridge, RateHold and export-driver signatures and manifest seals are refreshed before signing the app. Submit the final distributable using the existing `RysUpNotary` keychain profile; record acceptance, staple/validate it and calculate final hashes afterward.

Publish as a GitHub prerelease with the exact compatibility and evidence limitations above. Ship only our application/source, SDK notices, DMG and checksums; no vendor application, installed plugins, registry/license data, personal paths, test audio or private logs. No Shopify/commercial-plugin launch is implied.
